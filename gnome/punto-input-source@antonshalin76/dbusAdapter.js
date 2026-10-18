import GLib from 'gi://GLib';

import {Outcome, Protocol, wireUint64} from './inputSourceBridge.js';

function unpackParameters(parameters) {
    if (Array.isArray(parameters))
        return parameters;
    if (typeof parameters?.deepUnpack === 'function')
        return parameters.deepUnpack();
    throw new TypeError('invalid D-Bus method parameters');
}

function reply(invocation, signature, values) {
    invocation.return_value(new GLib.Variant(signature, values));
}

function failedResult(extensionInstance = '') {
    return [Outcome.Failed, extensionInstance, '', 0n, 0n,
        Protocol.Role.Unknown];
}

export function createDbusImplementation(bridgeCore) {
    if (bridgeCore === null || typeof bridgeCore !== 'object')
        throw new TypeError('bridge core is required');
    return {
        BindHelperAsync(parameters, invocation) {
            try {
                const [helperInstance, version, deadlineUs] =
                    unpackParameters(parameters);
                const result = bridgeCore.bindHelper({
                    sender: invocation.get_sender(),
                    helperInstance,
                    protocolVersion: version,
                    deadlineUs: wireUint64(deadlineUs),
                });
                reply(invocation, '(us)', [
                    result.outcome,
                    result.resultExtensionInstance,
                ]);
            } catch (_error) {
                reply(invocation, '(us)', [Outcome.Failed, '']);
            }
        },
        GetStateAsync(parameters, invocation) {
            try {
                const [helperInstance, clientInstance, deadlineUs] =
                    unpackParameters(parameters);
                const result = bridgeCore.getState({
                    sender: invocation.get_sender(),
                    helperInstance,
                    clientInstance,
                    deadlineUs: wireUint64(deadlineUs),
                });
                reply(invocation, '(ussttty)', [
                    result.outcome,
                    result.extensionInstance,
                    result.clientLeaseToken,
                    result.clientLeaseExpiryUs,
                    result.sourceGeneration,
                    result.stateGeneration,
                    result.currentRole,
                ]);
            } catch (_error) {
                reply(invocation, '(ussttty)', [
                    Outcome.Failed, '', '', 0n, 0n, 0n,
                    Protocol.Role.Unknown,
                ]);
            }
        },
        SwitchAsync(parameters, invocation) {
            try {
                const values = unpackParameters(parameters);
                const result = bridgeCore.switchSource({
                    helperInstance: values[0],
                    extensionInstance: values[1],
                    clientInstance: values[2],
                    clientLeaseToken: values[3],
                    sequence: wireUint64(values[4]),
                    requestDigest: values[5],
                    originalDeadlineUs: wireUint64(values[6]),
                    expectedSourceGeneration: wireUint64(values[7]),
                    expectedStateGeneration: wireUint64(values[8]),
                    expectedRole: values[9],
                    targetRole: values[10],
                }, invocation.get_sender());
                reply(invocation, '(usstty)', [
                    result.outcome,
                    result.resultExtensionInstance,
                    result.resultRequestDigest,
                    result.resultSourceGeneration,
                    result.resultStateGeneration,
                    result.resultRole,
                ]);
            } catch (_error) {
                reply(invocation, '(usstty)', failedResult());
            }
        },
        GetOperationAsync(parameters, invocation) {
            try {
                const values = unpackParameters(parameters);
                const result = bridgeCore.getOperation({
                    sender: invocation.get_sender(),
                    helperInstance: values[0],
                    extensionInstance: values[1],
                    clientInstance: values[2],
                    clientLeaseToken: values[3],
                    sequence: wireUint64(values[4]),
                    requestDigest: values[5],
                    queryDeadlineUs: wireUint64(values[6]),
                });
                reply(invocation, '(usstty)', [
                    result.outcome,
                    result.resultExtensionInstance,
                    result.resultRequestDigest,
                    result.resultSourceGeneration,
                    result.resultStateGeneration,
                    result.resultRole,
                ]);
            } catch (_error) {
                reply(invocation, '(usstty)', failedResult());
            }
        },
    };
}
