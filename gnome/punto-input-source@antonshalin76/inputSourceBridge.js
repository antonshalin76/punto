export const Outcome = Object.freeze({
    State: 0,
    GnomeStateApplied: 1,
    AlreadyCurrent: 2,
    UnsupportedState: 10,
    StaleState: 11,
    Conflict: 12,
    Expired: 13,
    Capacity: 14,
    Locked: 15,
    Diverged: 16,
    Failed: 17,
    NotFound: 18,
    VersionMismatch: 19,
    HelperStale: 20,
    DeadlineExpired: 21,
    HopUnknown: 22,
});

const Role = Object.freeze({EN: 0, RU: 1, Unknown: 255});

export const Protocol = Object.freeze({
    Version: 1,
    Role,
    MaxConnections: 4,
    MaxClients: 32,
    MaxOperations: 128,
    SwitchHorizonUs: 1_000_000n,
    ClientLeaseUs: 30_000_000n,
    ReconnectGraceUs: 2_500_000n,
});

export function canonicalRequestText(request) {
    return `version=${Protocol.Version}\n` +
        `extension_instance=${request.extensionInstance}\n` +
        `expected_source_generation=${request.expectedSourceGeneration}\n` +
        `expected_state_generation=${request.expectedStateGeneration}\n` +
        `expected_role=${request.expectedRole}\n` +
        `target_role=${request.targetRole}\n`;
}

export function wireUint64(value) {
    if (typeof value === 'bigint') {
        if (value < 0n || value > 18_446_744_073_709_551_615n)
            throw new TypeError('invalid uint64');
        return value;
    }
    if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0)
        throw new TypeError('invalid uint64');
    return BigInt(value);
}

const ID_PATTERN = /^[0-9a-f]{32}$/;
const DIGEST_PATTERN = /^[0-9a-f]{64}$/;

function validId(value) {
    return typeof value === 'string' && ID_PATTERN.test(value);
}

function validDigest(value) {
    return typeof value === 'string' && DIGEST_PATTERN.test(value);
}

function sameDigest(left, right) {
    if (!validDigest(left) || !validDigest(right))
        return false;
    let different = 0;
    for (let index = 0; index < left.length; index++)
        different |= left.charCodeAt(index) ^ right.charCodeAt(index);
    return different === 0;
}

function operationKey(extensionInstance, clientInstance, leaseToken, sequence) {
    return `${extensionInstance}:${clientInstance}:${leaseToken}:${sequence}`;
}

function leaseKey(clientInstance, leaseToken) {
    return `${clientInstance}:${leaseToken}`;
}

function terminal(outcome, extensionInstance, requestDigest = '',
    sourceGeneration = 0n, stateGeneration = 0n,
    resultRole = Role.Unknown) {
    return {
        outcome,
        resultExtensionInstance: extensionInstance,
        resultRequestDigest: requestDigest,
        resultSourceGeneration: sourceGeneration,
        resultStateGeneration: stateGeneration,
        resultRole,
    };
}

function sameSemantics(record, request) {
    return record.requestDigest === request.requestDigest &&
        record.originalDeadlineUs === request.originalDeadlineUs &&
        record.expectedSourceGeneration === request.expectedSourceGeneration &&
        record.expectedStateGeneration === request.expectedStateGeneration &&
        record.expectedRole === request.expectedRole &&
        record.targetRole === request.targetRole;
}

export class InputSourceBridgeCore {
    constructor({nowUs, randomId, helperOwner, snapshot, digestRequest}) {
        if (typeof nowUs !== 'function' || typeof randomId !== 'function' ||
            typeof helperOwner !== 'function' || typeof snapshot !== 'function' ||
            typeof digestRequest !== 'function')
            throw new TypeError('bridge dependencies are required');
        this._nowUs = nowUs;
        this._randomId = randomId;
        this._helperOwner = helperOwner;
        this._snapshot = snapshot;
        this._digestRequest = digestRequest;
        this._extensionInstance = this._newId();
        this._helperBinding = null;
        this._sourceGeneration = 1n;
        this._stateGeneration = 1n;
        this._leases = new Map();
        this._currentLease = new Map();
        this._operations = new Map();
        this._disabled = false;
    }

    disable() {
        this._disabled = true;
        this._helperBinding = null;
        this._leases.clear();
        this._currentLease.clear();
        this._operations.clear();
    }

    noteSourcesChanged() {
        this._sourceGeneration += 1n;
    }

    noteCurrentSourceChanged() {
        this._stateGeneration += 1n;
    }

    noteHelperOwnerChanged() {
        this._helperBinding = null;
    }

    bindHelper({sender, helperInstance, protocolVersion, deadlineUs}) {
        const now = this._nowUs();
        if (this._disabled || sender !== this._helperOwner() ||
            !validId(helperInstance))
            return terminal(Outcome.HelperStale, this._extensionInstance);
        if (protocolVersion !== Protocol.Version)
            return terminal(Outcome.VersionMismatch, this._extensionInstance);
        if (!this._validDeadline(deadlineUs, now))
            return terminal(Outcome.DeadlineExpired, this._extensionInstance);
        if (this._helperBinding !== null &&
            (this._helperBinding.sender !== sender ||
             this._helperBinding.instance !== helperInstance))
            return terminal(Outcome.HelperStale, this._extensionInstance);
        this._helperBinding = {sender, instance: helperInstance};
        return terminal(Outcome.State, this._extensionInstance);
    }

    getState({sender, helperInstance, clientInstance, deadlineUs}) {
        const now = this._nowUs();
        this._cleanup(now);
        if (!this._authorized(sender, helperInstance))
            return this._stateResult(Outcome.HelperStale);
        if (!validId(clientInstance))
            return this._stateResult(Outcome.Failed);
        if (!this._validDeadline(deadlineUs, now))
            return this._stateResult(Outcome.DeadlineExpired);
        const desktop = this._desktopState();
        if (desktop.outcome !== Outcome.State)
            return this._stateResult(desktop.outcome);
        if (desktop.locked)
            return this._stateResult(Outcome.Locked);

        let lease = this._activeLease(clientInstance, now);
        if (lease === null) {
            if (!this._currentLease.has(clientInstance) &&
                this._currentLease.size >= Protocol.MaxClients)
                return this._stateResult(Outcome.Capacity);
            const token = this._newId();
            lease = {
                clientInstance,
                token,
                expiryUs: now + Protocol.ClientLeaseUs,
                recoveryExpiryUs: now + Protocol.ClientLeaseUs +
                    Protocol.ReconnectGraceUs,
                highWater: 0n,
            };
            this._leases.set(leaseKey(clientInstance, token), lease);
            this._currentLease.set(clientInstance, token);
        }
        return {
            outcome: Outcome.State,
            extensionInstance: this._extensionInstance,
            clientLeaseToken: lease.token,
            clientLeaseExpiryUs: lease.expiryUs,
            sourceGeneration: this._sourceGeneration,
            stateGeneration: this._stateGeneration,
            currentRole: desktop.currentRole,
        };
    }

    switchSource(request, sender) {
        const now = this._nowUs();
        this._cleanup(now);
        if (!this._authorized(sender, request.helperInstance))
            return terminal(Outcome.HelperStale, this._extensionInstance);
        if (!this._validSwitchRequest(request))
            return terminal(Outcome.Failed, this._extensionInstance);
        let computedDigest;
        try {
            computedDigest = this._digestRequest(canonicalRequestText(request));
        } catch (_error) {
            return terminal(Outcome.Failed, this._extensionInstance);
        }
        if (!sameDigest(computedDigest, request.requestDigest))
            return terminal(Outcome.Conflict, this._extensionInstance,
                request.requestDigest);
        if (request.extensionInstance !== this._extensionInstance)
            return terminal(Outcome.StaleState, this._extensionInstance,
                request.requestDigest);

        const lease = this._leases.get(leaseKey(
            request.clientInstance, request.clientLeaseToken));
        if (lease === undefined || now >= lease.expiryUs ||
            this._currentLease.get(request.clientInstance) !== lease.token)
            return terminal(Outcome.Expired, this._extensionInstance,
                request.requestDigest);
        const key = operationKey(this._extensionInstance,
            request.clientInstance, request.clientLeaseToken, request.sequence);
        const previous = this._operations.get(key);
        if (previous !== undefined) {
            if (!sameSemantics(previous, request))
                return terminal(Outcome.Conflict, this._extensionInstance,
                    request.requestDigest);
            return previous.result;
        }
        if (request.sequence <= lease.highWater)
            return terminal(Outcome.Expired, this._extensionInstance,
                request.requestDigest);
        lease.highWater = request.sequence;
        if (!this._validDeadline(request.originalDeadlineUs, now))
            return terminal(Outcome.DeadlineExpired, this._extensionInstance,
                request.requestDigest);
        if (this._operations.size >= Protocol.MaxOperations)
            return terminal(Outcome.Capacity, this._extensionInstance,
                request.requestDigest);

        const desktop = this._desktopState();
        if (desktop.outcome !== Outcome.State)
            return terminal(desktop.outcome, this._extensionInstance,
                request.requestDigest);
        if (desktop.locked)
            return terminal(Outcome.Locked, this._extensionInstance,
                request.requestDigest);
        if (request.expectedSourceGeneration !== this._sourceGeneration ||
            request.expectedStateGeneration !== this._stateGeneration ||
            request.expectedRole !== desktop.currentRole)
            return terminal(Outcome.StaleState, this._extensionInstance,
                request.requestDigest, this._sourceGeneration,
                this._stateGeneration, desktop.currentRole);

        const record = {
            requestDigest: request.requestDigest,
            originalDeadlineUs: request.originalDeadlineUs,
            expectedSourceGeneration: request.expectedSourceGeneration,
            expectedStateGeneration: request.expectedStateGeneration,
            expectedRole: request.expectedRole,
            targetRole: request.targetRole,
            expiresUs: request.originalDeadlineUs + Protocol.ReconnectGraceUs,
            result: terminal(Outcome.Failed, this._extensionInstance,
                request.requestDigest, this._sourceGeneration,
                this._stateGeneration, desktop.currentRole),
        };
        this._operations.set(key, record);

        if (request.targetRole === desktop.currentRole) {
            record.result = terminal(Outcome.AlreadyCurrent,
                this._extensionInstance, request.requestDigest,
                this._sourceGeneration, this._stateGeneration,
                desktop.currentRole);
            return record.result;
        }
        if (request.targetRole !== Role.EN && request.targetRole !== Role.RU) {
            record.result = terminal(Outcome.UnsupportedState,
                this._extensionInstance, request.requestDigest);
            return record.result;
        }
        if (this._nowUs() >= request.originalDeadlineUs) {
            record.result = terminal(Outcome.DeadlineExpired,
                this._extensionInstance, request.requestDigest);
            return record.result;
        }

        try {
            desktop.byRole.get(request.targetRole).activate(false);
        } catch (_error) {
            record.result = terminal(Outcome.Failed, this._extensionInstance,
                request.requestDigest);
            return record.result;
        }
        const after = this._desktopState();
        const applied = after.outcome === Outcome.State && !after.locked &&
            after.currentRole === request.targetRole &&
            after.currentLayoutRole === request.targetRole;
        record.result = terminal(applied ? Outcome.GnomeStateApplied :
            Outcome.Diverged, this._extensionInstance, request.requestDigest,
        this._sourceGeneration, this._stateGeneration,
        after.outcome === Outcome.State ? after.currentRole : Role.Unknown);
        return record.result;
    }

    getOperation(request) {
        const now = this._nowUs();
        this._cleanup(now);
        if (!this._authorized(request.sender, request.helperInstance))
            return terminal(Outcome.HelperStale, this._extensionInstance);
        if (!validId(request.extensionInstance) ||
            !validId(request.clientInstance) ||
            !validId(request.clientLeaseToken) ||
            typeof request.sequence !== 'bigint' || request.sequence === 0n ||
            !validDigest(request.requestDigest) ||
            typeof request.queryDeadlineUs !== 'bigint')
            return terminal(Outcome.Failed, this._extensionInstance);
        if (!this._validDeadline(request.queryDeadlineUs, now))
            return terminal(Outcome.DeadlineExpired, this._extensionInstance,
                request.requestDigest);
        if (request.extensionInstance !== this._extensionInstance)
            return terminal(Outcome.StaleState, this._extensionInstance,
                request.requestDigest);
        const key = operationKey(this._extensionInstance,
            request.clientInstance, request.clientLeaseToken, request.sequence);
        const record = this._operations.get(key);
        if (record === undefined) {
            const lease = this._leases.get(leaseKey(
                request.clientInstance, request.clientLeaseToken));
            return terminal(lease === undefined || now >= lease.recoveryExpiryUs ||
                request.sequence <= lease.highWater ?
                Outcome.Expired : Outcome.NotFound,
            this._extensionInstance, request.requestDigest);
        }
        if (record.requestDigest !== request.requestDigest)
            return terminal(Outcome.Conflict, this._extensionInstance,
                request.requestDigest);
        return record.result;
    }

    _newId() {
        const value = this._randomId();
        if (!validId(value))
            throw new TypeError('randomId must return 128-bit lowercase hex');
        return value;
    }

    _validDeadline(deadlineUs, now) {
        return typeof deadlineUs === 'bigint' && deadlineUs > now &&
            deadlineUs - now <= Protocol.SwitchHorizonUs;
    }

    _validSwitchRequest(request) {
        return validId(request.helperInstance) &&
            validId(request.extensionInstance) &&
            validId(request.clientInstance) &&
            validId(request.clientLeaseToken) &&
            typeof request.sequence === 'bigint' && request.sequence > 0n &&
            validDigest(request.requestDigest) &&
            typeof request.originalDeadlineUs === 'bigint' &&
            typeof request.expectedSourceGeneration === 'bigint' &&
            typeof request.expectedStateGeneration === 'bigint' &&
            Number.isInteger(request.expectedRole) &&
            Number.isInteger(request.targetRole);
    }

    _authorized(sender, helperInstance) {
        return !this._disabled && this._helperBinding !== null &&
            sender === this._helperOwner() &&
            sender === this._helperBinding.sender &&
            helperInstance === this._helperBinding.instance;
    }

    _activeLease(clientInstance, now) {
        const token = this._currentLease.get(clientInstance);
        if (token === undefined)
            return null;
        const lease = this._leases.get(leaseKey(clientInstance, token));
        return lease !== undefined && now < lease.expiryUs ? lease : null;
    }

    _desktopState() {
        let value;
        try {
            value = this._snapshot();
        } catch (_error) {
            return {outcome: Outcome.Failed};
        }
        if (value === null || typeof value !== 'object' ||
            !Array.isArray(value.sources) || value.sources.length !== 2)
            return {outcome: Outcome.UnsupportedState};
        const byRole = new Map();
        for (const source of value.sources) {
            if (source === null || source.type !== 'xkb' ||
                (source.id !== 'us' && source.id !== 'ru') ||
                source.xkbId !== source.id ||
                typeof source.activate !== 'function')
                return {outcome: Outcome.UnsupportedState};
            const role = source.id === 'us' ? Role.EN : Role.RU;
            if (byRole.has(role))
                return {outcome: Outcome.UnsupportedState};
            byRole.set(role, source);
        }
        if (byRole.size !== 2 || !value.currentSource)
            return {outcome: Outcome.UnsupportedState};
        const currentRole = value.currentSource.id === 'us' ? Role.EN :
            value.currentSource.id === 'ru' ? Role.RU : Role.Unknown;
        if (currentRole === Role.Unknown ||
            value.currentSource !== byRole.get(currentRole) ||
            value.currentSource.type !== 'xkb' ||
            value.currentSource.xkbId !== value.currentSource.id)
            return {outcome: Outcome.UnsupportedState};
        if (value.currentLayout === null ||
            typeof value.currentLayout !== 'object' ||
            typeof value.currentLayout.id !== 'string' ||
            typeof value.currentLayout.layout !== 'string' ||
            value.currentLayout.id !== value.currentLayout.layout ||
            value.currentLayout.variant !== '')
            return {outcome: Outcome.UnsupportedState};
        const layoutId = value.currentLayout.id;
        const currentLayoutRole = layoutId === 'us' ? Role.EN :
            layoutId === 'ru' ? Role.RU : Role.Unknown;
        if (currentLayoutRole === Role.Unknown ||
            currentLayoutRole !== currentRole)
            return {outcome: Outcome.Diverged};
        return {
            outcome: Outcome.State,
            byRole,
            currentRole,
            currentLayoutRole,
            locked: value.locked === true,
        };
    }

    _stateResult(outcome) {
        return {
            outcome,
            extensionInstance: this._extensionInstance,
            clientLeaseToken: '',
            clientLeaseExpiryUs: 0n,
            sourceGeneration: this._sourceGeneration,
            stateGeneration: this._stateGeneration,
            currentRole: Role.Unknown,
        };
    }

    _cleanup(now) {
        for (const [key, record] of this._operations) {
            if (now >= record.expiresUs)
                this._operations.delete(key);
        }
        for (const [key, lease] of this._leases) {
            if (now < lease.recoveryExpiryUs)
                continue;
            this._leases.delete(key);
            if (this._currentLease.get(lease.clientInstance) === lease.token)
                this._currentLease.delete(lease.clientInstance);
        }
    }
}
