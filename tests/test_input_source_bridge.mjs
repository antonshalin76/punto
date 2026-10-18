import {
    canonicalRequestText,
    InputSourceBridgeCore,
    Outcome,
    Protocol,
} from '../gnome/punto-input-source@antonshalin76/inputSourceBridge.js';

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function digestRequest(value) {
    let hash = 2166136261;
    for (const character of value) {
        hash ^= character.codePointAt(0);
        hash = Math.imul(hash, 16777619) >>> 0;
    }
    return hash.toString(16).padStart(8, '0').repeat(8);
}

function request(overrides = {}) {
    const value = {
        helperInstance: '22'.repeat(16),
        extensionInstance: '11'.repeat(16),
        clientInstance: '33'.repeat(16),
        clientLeaseToken: '44'.repeat(16),
        sequence: 1n,
        originalDeadlineUs: 1_500_000n,
        expectedSourceGeneration: 1n,
        expectedStateGeneration: 1n,
        expectedRole: Protocol.Role.EN,
        targetRole: Protocol.Role.RU,
        ...overrides,
    };
    if (!Object.hasOwn(overrides, 'requestDigest'))
        value.requestDigest = digestRequest(canonicalRequestText(value));
    return value;
}

function fixture(order = ['us', 'ru'], requireState = true) {
    let now = 1_000_000n;
    let owner = ':1.42';
    let locked = false;
    let activeId = 'us';
    let currentLayout = {id: 'us', layout: 'us', variant: ''};
    let currentSourceOverride = null;
    let activationCount = 0;
    let activationEnabled = true;
    let activationThrows = false;
    let postActivationLayout = null;
    let advanceOnSnapshotUs = 0n;
    let nextId = 1;
    let core;
    const sources = order.map((id, index) => ({
        type: 'xkb',
        id,
        xkbId: id,
        index,
        activate(interactive) {
            assert(interactive === false, 'bridge activation must be noninteractive');
            if (activationThrows)
                throw new Error('injected activation failure');
            if (!activationEnabled)
                return;
            activationCount += 1;
            activeId = id;
            currentLayout = postActivationLayout ??
                {id, layout: id, variant: ''};
            core.noteCurrentSourceChanged();
        },
    }));
    const snapshot = () => {
        now += advanceOnSnapshotUs;
        advanceOnSnapshotUs = 0n;
        return {
            sources,
            currentSource: currentSourceOverride ??
                sources.find(source => source.id === activeId),
            currentLayout,
            locked,
        };
    };
    core = new InputSourceBridgeCore({
        nowUs: () => now,
        randomId: () => (nextId++).toString(16).padStart(32, '0'),
        helperOwner: () => owner,
        snapshot,
        digestRequest,
    });
    const bind = core.bindHelper({
        sender: owner,
        helperInstance: '22'.repeat(16),
        protocolVersion: Protocol.Version,
        deadlineUs: now + 100_000n,
    });
    assert(bind.outcome === Outcome.State, 'helper bind failed');
    const state = core.getState({
        sender: owner,
        helperInstance: '22'.repeat(16),
        clientInstance: '33'.repeat(16),
        deadlineUs: now + 100_000n,
    });
    if (requireState)
        assert(state.outcome === Outcome.State, 'initial state failed');
    return {
        core,
        owner: () => owner,
        setOwner(value) {
            owner = value;
            core.noteHelperOwnerChanged();
        },
        setLocked(value) { locked = value; },
        setActive(value) {
            activeId = value;
            currentLayout = {id: value, layout: value, variant: ''};
            core.noteCurrentSourceChanged();
        },
        setLayout(value) {
            currentLayout = {id: value, layout: value, variant: ''};
        },
        setLayoutObject(value) { currentLayout = value; },
        setCurrentSource(value) { currentSourceOverride = value; },
        breakActivation() { activationEnabled = false; },
        throwActivation() { activationThrows = true; },
        malformAfterActivation(value) { postActivationLayout = value; },
        advanceOnNextSnapshot(value) { advanceOnSnapshotUs = value; },
        advance(value) { now += value; },
        now: () => now,
        activations: () => activationCount,
        state,
    };
}

function switchFrom(f, overrides = {}) {
    return f.core.switchSource(request({
        extensionInstance: f.state.extensionInstance,
        clientLeaseToken: f.state.clientLeaseToken,
        expectedSourceGeneration: f.state.sourceGeneration,
        expectedStateGeneration: f.state.stateGeneration,
        originalDeadlineUs: f.now() + 500_000n,
        ...overrides,
    }), f.owner());
}

function testSupportedSourcesAndCas() {
    for (const order of [['us', 'ru'], ['ru', 'us']]) {
        const f = fixture(order);
        assert(f.state.currentRole === Protocol.Role.EN,
            'source role must not depend on configured order');
        const result = switchFrom(f);
        assert(result.outcome === Outcome.GnomeStateApplied,
            'valid CAS did not produce GNOME terminal result');
        assert(result.resultRole === Protocol.Role.RU && f.activations() === 1,
            'valid CAS did not select RU exactly once');
        const repeated = switchFrom(f);
        assert(repeated.outcome === Outcome.GnomeStateApplied &&
            f.activations() === 1, 'idempotent replay activated twice');
        const conflict = switchFrom(f, {
            originalDeadlineUs: f.now() + 600_000n,
        });
        assert(conflict.outcome === Outcome.Conflict && f.activations() === 1,
            'changed immutable deadline was not rejected');
    }
}

function testUnsupportedAndUnauthorized() {
    for (const order of [
        ['us'],
        ['us', 'ru', 'de'],
        ['us', 'us'],
    ]) {
        const f = fixture(order, false);
        const state = f.core.getState({
            sender: f.owner(), helperInstance: '22'.repeat(16),
            clientInstance: '66'.repeat(16), deadlineUs: f.now() + 10n,
        });
        assert(state.outcome === Outcome.UnsupportedState,
            `unsupported sources accepted: ${order.join(',')}`);
    }
    const f = fixture();
    const foreign = f.core.getState({
        sender: ':1.99', helperInstance: '22'.repeat(16),
        clientInstance: '66'.repeat(16), deadlineUs: f.now() + 10n,
    });
    assert(foreign.outcome === Outcome.HelperStale,
        'direct non-helper session caller was accepted');
    f.setLocked(true);
    const locked = switchFrom(f);
    assert(locked.outcome === Outcome.Locked && f.activations() === 0,
        'locked session mutated the source');
    f.setLocked(false);
    const unlockedReplay = switchFrom(f);
    assert(unlockedReplay.outcome === Outcome.Expired && f.activations() === 0,
        'locked request revived after unlock');
    const recovered = switchFrom(f, {sequence: 2n});
    assert(recovered.outcome === Outcome.GnomeStateApplied &&
        f.activations() === 1,
    'new operation did not recover after safe lock rejection');
}

function testAlreadyCurrentAndDivergence() {
    const f = fixture();
    const already = switchFrom(f, {
        expectedRole: Protocol.Role.EN,
        targetRole: Protocol.Role.EN,
    });
    assert(already.outcome === Outcome.AlreadyCurrent && f.activations() === 0,
        'AlreadyCurrent must not activate');

    const broken = fixture();
    broken.breakActivation();
    const result = switchFrom(broken);
    assert(result.outcome === Outcome.Diverged && broken.activations() === 0,
        'native no-op was reported as applied');
    const repeated = switchFrom(broken);
    assert(repeated.outcome === Outcome.Diverged && broken.activations() === 0,
        'terminal divergence was retried');

    const malformedAfter = fixture();
    malformedAfter.malformAfterActivation({
        id: 'ru', layout: 'us', variant: '',
    });
    const malformedResult = switchFrom(malformedAfter);
    assert(malformedResult.outcome === Outcome.Diverged &&
        malformedAfter.activations() === 1,
    'malformed post-activation layout was reported as applied');

    const mismatch = fixture();
    mismatch.setLayout('ru');
    const mismatchState = mismatch.core.getState({
        sender: mismatch.owner(), helperInstance: '22'.repeat(16),
        clientInstance: '66'.repeat(16), deadlineUs: mismatch.now() + 10n,
    });
    assert(mismatchState.outcome === Outcome.Diverged,
        'source/layout mismatch was accepted by GetState');
    const mismatchSwitch = switchFrom(mismatch, {
        expectedRole: Protocol.Role.EN,
        targetRole: Protocol.Role.EN,
    });
    assert(mismatchSwitch.outcome === Outcome.Diverged &&
        mismatch.activations() === 0,
    'source/layout mismatch produced AlreadyCurrent');

    const staleSource = fixture();
    staleSource.setCurrentSource({
        type: 'xkb', id: 'us', xkbId: 'us', activate() {},
    });
    const staleState = staleSource.core.getState({
        sender: staleSource.owner(), helperInstance: '22'.repeat(16),
        clientInstance: '66'.repeat(16), deadlineUs: staleSource.now() + 10n,
    });
    assert(staleState.outcome === Outcome.UnsupportedState,
        'source outside validated collection was accepted');

    for (const layout of [
        {id: 'us', layout: 'ru', variant: ''},
        {id: 'us', layout: 'us', variant: 'intl'},
        {id: 'us', variant: ''},
        {variant: ''},
        null,
    ]) {
        const malformed = fixture();
        malformed.setLayoutObject(layout);
        const malformedState = malformed.core.getState({
            sender: malformed.owner(), helperInstance: '22'.repeat(16),
            clientInstance: '66'.repeat(16),
            deadlineUs: malformed.now() + 10n,
        });
        assert(malformedState.outcome === Outcome.UnsupportedState,
            `malformed currentLayout was accepted: ${JSON.stringify(layout)}`);
    }
}

function testDigestValidation() {
    const f = fixture();
    const staleDigest = request({
        extensionInstance: f.state.extensionInstance,
        clientLeaseToken: f.state.clientLeaseToken,
        originalDeadlineUs: f.now() + 500_000n,
        expectedSourceGeneration: f.state.sourceGeneration,
        expectedStateGeneration: f.state.stateGeneration,
    });
    staleDigest.targetRole = Protocol.Role.EN;
    const result = f.core.switchSource(staleDigest, f.owner());
    assert(result.outcome === Outcome.Conflict && f.activations() === 0,
        'semantic mutation with stale digest was accepted');
}

function testBindingAndDisableLifecycle() {
    const f = fixture();
    const replacement = f.core.bindHelper({
        sender: f.owner(), helperInstance: '77'.repeat(16),
        protocolVersion: Protocol.Version,
        deadlineUs: f.now() + 100_000n,
    });
    assert(replacement.outcome === Outcome.HelperStale,
        'live helper binding was replaced in place');
    f.core.disable();
    f.core.noteSourcesChanged();
    f.core.noteCurrentSourceChanged();
    f.core.noteHelperOwnerChanged();
    const disabled = f.core.getState({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        clientInstance: '33'.repeat(16), deadlineUs: f.now() + 10n,
    });
    assert(disabled.outcome === Outcome.HelperStale,
        'late callback revived disabled bridge');
}

function testCasAndNativeFailureAreTerminal() {
    const f = fixture();
    const stale = switchFrom(f, {expectedStateGeneration: 99n});
    assert(stale.outcome === Outcome.StaleState && f.activations() === 0,
        'state-generation CAS loser activated');
    const staleReplay = switchFrom(f, {expectedStateGeneration: 99n});
    assert(staleReplay.outcome === Outcome.Expired && f.activations() === 0,
        'state-generation rejection allowed the same sequence to revive');

    const sourceStale = fixture();
    const staleSource = switchFrom(sourceStale, {
        expectedSourceGeneration: 99n,
    });
    assert(staleSource.outcome === Outcome.StaleState &&
        sourceStale.activations() === 0,
    'source-generation CAS loser activated');

    const throwing = fixture();
    throwing.throwActivation();
    const failed = switchFrom(throwing);
    assert(failed.outcome === Outcome.Failed && throwing.activations() === 0,
        'native activation throw was not terminal');
    const repeated = switchFrom(throwing);
    assert(repeated.outcome === Outcome.Failed && throwing.activations() === 0,
        'native activation throw was retried');

    const expiring = fixture();
    expiring.advanceOnNextSnapshot(600_000n);
    const deadline = switchFrom(expiring);
    assert(deadline.outcome === Outcome.DeadlineExpired &&
        expiring.activations() === 0,
    'deadline crossing during preflight reached native activation');
    const deadlineReplay = switchFrom(expiring, {
        originalDeadlineUs: expiring.now() + 500_000n,
    });
    assert(deadlineReplay.outcome === Outcome.Conflict &&
        expiring.activations() === 0,
    'deadline-crossing operation revived with an extended deadline');
}

function testOperationRetentionDoesNotCausePeriodicOutage() {
    const f = fixture();
    let sequence = 1n;
    for (let index = 0; index < 150; index++) {
        const state = f.core.getState({
            sender: f.owner(), helperInstance: '22'.repeat(16),
            clientInstance: '33'.repeat(16), deadlineUs: f.now() + 100_000n,
        });
        assert(state.outcome === Outcome.State,
            `state admission failed during sustained correction ${index}`);
        const targetRole = state.currentRole === Protocol.Role.EN ?
            Protocol.Role.RU : Protocol.Role.EN;
        const result = f.core.switchSource(request({
            extensionInstance: state.extensionInstance,
            clientLeaseToken: state.clientLeaseToken,
            sequence,
            originalDeadlineUs: f.now() + 500_000n,
            expectedSourceGeneration: state.sourceGeneration,
            expectedStateGeneration: state.stateGeneration,
            expectedRole: state.currentRole,
            targetRole,
        }), f.owner());
        assert(result.outcome === Outcome.GnomeStateApplied,
            `operation journal caused outage at correction ${index}`);
        sequence += 1n;
        f.advance(30_000n);
    }
    assert(f.activations() === 150,
        'sustained correction stream lost an activation');
}

function testCapacityAndHighWaterRecovery() {
    const f = fixture();
    let lastState = f.state;
    for (let index = 1; index <= Protocol.MaxOperations; index++) {
        if (index > 1) {
            lastState = f.core.getState({
                sender: f.owner(), helperInstance: '22'.repeat(16),
                clientInstance: '33'.repeat(16),
                deadlineUs: f.now() + 100_000n,
            });
        }
        const targetRole = lastState.currentRole === Protocol.Role.EN ?
            Protocol.Role.RU : Protocol.Role.EN;
        const result = f.core.switchSource(request({
            extensionInstance: lastState.extensionInstance,
            clientLeaseToken: lastState.clientLeaseToken,
            sequence: BigInt(index),
            originalDeadlineUs: f.now() + 500_000n,
            expectedSourceGeneration: lastState.sourceGeneration,
            expectedStateGeneration: lastState.stateGeneration,
            expectedRole: lastState.currentRole,
            targetRole,
        }), f.owner());
        assert(result.outcome === Outcome.GnomeStateApplied,
            `journal fill failed early at ${index}`);
    }
    const fullState = f.core.getState({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        clientInstance: '33'.repeat(16), deadlineUs: f.now() + 100_000n,
    });
    const rejected = f.core.switchSource(request({
        extensionInstance: fullState.extensionInstance,
        clientLeaseToken: fullState.clientLeaseToken,
        sequence: 129n,
        originalDeadlineUs: f.now() + 500_000n,
        expectedSourceGeneration: fullState.sourceGeneration,
        expectedStateGeneration: fullState.stateGeneration,
        expectedRole: fullState.currentRole,
        targetRole: Protocol.Role.RU,
    }), f.owner());
    assert(rejected.outcome === Outcome.Capacity,
        'full journal did not reject before activation');
    const rejectedAgain = f.core.switchSource(request({
        extensionInstance: fullState.extensionInstance,
        clientLeaseToken: fullState.clientLeaseToken,
        sequence: 129n,
        originalDeadlineUs: f.now() + 500_000n,
        expectedSourceGeneration: fullState.sourceGeneration,
        expectedStateGeneration: fullState.stateGeneration,
        expectedRole: fullState.currentRole,
        targetRole: Protocol.Role.RU,
    }), f.owner());
    assert(rejectedAgain.outcome === Outcome.Expired,
        'capacity rejection did not consume the operation sequence');

    f.advance(Protocol.SwitchHorizonUs + Protocol.ReconnectGraceUs + 1n);
    const evicted = f.core.getOperation({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        extensionInstance: f.state.extensionInstance,
        clientInstance: '33'.repeat(16),
        clientLeaseToken: f.state.clientLeaseToken,
        sequence: 1n,
        requestDigest: request({
            extensionInstance: f.state.extensionInstance,
            expectedSourceGeneration: f.state.sourceGeneration,
            expectedStateGeneration: f.state.stateGeneration,
        }).requestDigest,
        queryDeadlineUs: f.now() + 100_000n,
    });
    assert(evicted.outcome === Outcome.Expired,
        'evicted sequence below retained high-water was not Expired');

    const recoveredState = f.core.getState({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        clientInstance: '33'.repeat(16), deadlineUs: f.now() + 100_000n,
    });
    const recoveredTarget = recoveredState.currentRole === Protocol.Role.EN ?
        Protocol.Role.RU : Protocol.Role.EN;
    const recovered = f.core.switchSource(request({
        extensionInstance: recoveredState.extensionInstance,
        clientLeaseToken: recoveredState.clientLeaseToken,
        sequence: 130n,
        originalDeadlineUs: f.now() + 500_000n,
        expectedSourceGeneration: recoveredState.sourceGeneration,
        expectedStateGeneration: recoveredState.stateGeneration,
        expectedRole: recoveredState.currentRole,
        targetRole: recoveredTarget,
    }), f.owner());
    assert(recovered.outcome === Outcome.GnomeStateApplied,
        'journal did not recover after bounded retention');
}

function testClientCapacityRecovery() {
    const f = fixture();
    for (let index = 0; index < Protocol.MaxClients - 1; index++) {
        const clientInstance = BigInt(index + 100).toString(16).padStart(32, '0');
        const state = f.core.getState({
            sender: f.owner(), helperInstance: '22'.repeat(16),
            clientInstance, deadlineUs: f.now() + 100_000n,
        });
        assert(state.outcome === Outcome.State,
            `client table filled early at ${index}`);
    }
    const extraClient = 'ee'.repeat(16);
    const full = f.core.getState({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        clientInstance: extraClient, deadlineUs: f.now() + 100_000n,
    });
    assert(full.outcome === Outcome.Capacity,
        'full client table accepted another client');
    f.advance(Protocol.ClientLeaseUs + Protocol.ReconnectGraceUs + 1n);
    const recovered = f.core.getState({
        sender: f.owner(), helperInstance: '22'.repeat(16),
        clientInstance: extraClient, deadlineUs: f.now() + 100_000n,
    });
    assert(recovered.outcome === Outcome.State,
        'client capacity did not recover after expiry and grace');
}

function testDeadlineLeaseAndHelperReplacement() {
    const f = fixture();
    const expired = switchFrom(f, {originalDeadlineUs: f.now()});
    assert(expired.outcome === Outcome.DeadlineExpired && f.activations() === 0,
        'expired request activated');
    const expiredReplay = switchFrom(f, {
        originalDeadlineUs: f.now() + 500_000n,
    });
    assert(expiredReplay.outcome === Outcome.Expired && f.activations() === 0,
        'expired operation revived with an extended deadline');

    const tooFar = switchFrom(f, {
        sequence: 2n,
        originalDeadlineUs: f.now() + Protocol.SwitchHorizonUs + 1n,
    });
    assert(tooFar.outcome === Outcome.DeadlineExpired && f.activations() === 0,
        'beyond-horizon request activated');
    const shortenedReplay = switchFrom(f, {sequence: 2n});
    assert(shortenedReplay.outcome === Outcome.Expired && f.activations() === 0,
        'beyond-horizon operation revived with a shortened deadline');

    f.advance(Protocol.ClientLeaseUs - 400_000n);
    const appliedRequest = request({
        extensionInstance: f.state.extensionInstance,
        clientLeaseToken: f.state.clientLeaseToken,
        sequence: 3n,
        originalDeadlineUs: f.now() + 500_000n,
        expectedSourceGeneration: f.state.sourceGeneration,
        expectedStateGeneration: f.state.stateGeneration,
    });
    const applied = f.core.switchSource(appliedRequest, f.owner());
    assert(applied.outcome === Outcome.GnomeStateApplied && f.activations() === 1,
        'recovery operation did not apply');
    f.advance(400_001n);
    const forbiddenRetry = f.core.switchSource(appliedRequest, f.owner());
    assert(forbiddenRetry.outcome === Outcome.Expired && f.activations() === 1,
        'expired lease replayed or exposed its terminal result via Switch');
    const historical = f.core.getOperation({
        sender: f.owner(),
        helperInstance: '22'.repeat(16),
        extensionInstance: f.state.extensionInstance,
        clientInstance: '33'.repeat(16),
        clientLeaseToken: f.state.clientLeaseToken,
        sequence: 3n,
        requestDigest: appliedRequest.requestDigest,
        queryDeadlineUs: f.now() + 100_000n,
    });
    assert(historical.outcome === Outcome.GnomeStateApplied,
        'expired lease could not read terminal result during grace');

    f.setOwner(':1.43');
    const rebound = f.core.bindHelper({
        sender: ':1.43',
        helperInstance: '77'.repeat(16),
        protocolVersion: Protocol.Version,
        deadlineUs: f.now() + 100_000n,
    });
    assert(rebound.outcome === Outcome.State, 'replacement helper did not bind');
    const recovered = f.core.getOperation({
        sender: ':1.43', helperInstance: '77'.repeat(16),
        extensionInstance: f.state.extensionInstance,
        clientInstance: '33'.repeat(16),
        clientLeaseToken: f.state.clientLeaseToken,
        sequence: 3n, requestDigest: appliedRequest.requestDigest,
        queryDeadlineUs: f.now() + 100_000n,
    });
    assert(recovered.outcome === Outcome.GnomeStateApplied &&
        f.activations() === 1, 'helper replacement repeated or lost operation');

    f.advance(Protocol.ReconnectGraceUs + 100_001n);
    const gone = f.core.getOperation({
        sender: ':1.43', helperInstance: '77'.repeat(16),
        extensionInstance: f.state.extensionInstance,
        clientInstance: '33'.repeat(16),
        clientLeaseToken: f.state.clientLeaseToken,
        sequence: 3n, requestDigest: appliedRequest.requestDigest,
        queryDeadlineUs: f.now() + 100_000n,
    });
    assert(gone.outcome === Outcome.Expired,
        'historical lease survived reconnect grace');
}

function testSchemaConstants() {
    assert(Protocol.Version === 1 && Protocol.MaxConnections === 4 &&
        Protocol.MaxClients === 32 && Protocol.MaxOperations === 128,
    'protocol capacity constants drifted');
    assert(Protocol.SwitchHorizonUs === 1_000_000n &&
        Protocol.ClientLeaseUs === 30_000_000n &&
        Protocol.ReconnectGraceUs === 2_500_000n,
    'protocol deadline constants drifted');
    const outcomes = [
        Outcome.State, Outcome.GnomeStateApplied, Outcome.AlreadyCurrent,
        Outcome.UnsupportedState, Outcome.StaleState, Outcome.Conflict,
        Outcome.Expired, Outcome.Capacity, Outcome.Locked, Outcome.Diverged,
        Outcome.Failed, Outcome.NotFound, Outcome.VersionMismatch,
        Outcome.HelperStale, Outcome.DeadlineExpired, Outcome.HopUnknown,
    ];
    assert(outcomes.join(',') ===
        '0,1,2,10,11,12,13,14,15,16,17,18,19,20,21,22',
    'wire outcome values drifted');
    assert([Protocol.Role.EN, Protocol.Role.RU, Protocol.Role.Unknown].join(',') ===
        '0,1,255', 'wire role values drifted');
    const canonical = canonicalRequestText({
        extensionInstance: 'ab'.repeat(16),
        expectedSourceGeneration: 7n,
        expectedStateGeneration: 8n,
        expectedRole: Protocol.Role.EN,
        targetRole: Protocol.Role.RU,
    });
    assert(canonical === 'version=1\nextension_instance=' + 'ab'.repeat(16) +
        '\nexpected_source_generation=7\nexpected_state_generation=8\n' +
        'expected_role=0\ntarget_role=1\n',
    'canonical digest serialization drifted');
}

testSupportedSourcesAndCas();
testUnsupportedAndUnauthorized();
testAlreadyCurrentAndDivergence();
testDigestValidation();
testBindingAndDisableLifecycle();
testCasAndNativeFailureAreTerminal();
testOperationRetentionDoesNotCausePeriodicOutage();
testCapacityAndHighWaterRecovery();
testClientCapacityRecovery();
testDeadlineLeaseAndHelperReplacement();
testSchemaConstants();
console.log('test_input_source_bridge: OK');
