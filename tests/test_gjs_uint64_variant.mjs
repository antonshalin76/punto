import GLib from 'gi://GLib';

import {canonicalRequestText, wireUint64} from
    '../gnome/punto-input-source@antonshalin76/inputSourceBridge.js';

const unpacked = new GLib.Variant('(t)', [1n]).deepUnpack()[0];
if (typeof unpacked !== 'number')
    throw new Error('GNOME 46 uint64 fixture no longer returns Number');
if (wireUint64(unpacked) !== 1n)
    throw new Error('uint64 adapter did not preserve the value');
const maxSafe = new GLib.Variant(
    '(t)', [BigInt(Number.MAX_SAFE_INTEGER)]).deepUnpack()[0];
if (wireUint64(maxSafe) !== BigInt(Number.MAX_SAFE_INTEGER))
    throw new Error('maximum safe GVariant uint64 was not preserved');
const firstUnsafe = new GLib.Variant(
    '(t)', [BigInt(Number.MAX_SAFE_INTEGER) + 1n]).deepUnpack()[0];
let unsafeVariantRejected = false;
try {
    wireUint64(firstUnsafe);
} catch (_error) {
    unsafeVariantRejected = true;
}
if (!unsafeVariantRejected)
    throw new Error('unsafe GVariant uint64 was accepted');

const stateReply = new GLib.Variant('(ussttty)', [
    0, '11'.repeat(16), '22'.repeat(16), 30_000_000n, 7n, 8n, 1,
]).deepUnpack();
if (stateReply.slice(3, 6).map(wireUint64).join(',') !== '30000000,7,8')
    throw new Error('GetState uint64 tuple fields drifted');
const operationReply = new GLib.Variant('(usstty)', [
    1, '11'.repeat(16), 'aa'.repeat(32), 7n, 8n, 1,
]).deepUnpack();
if (operationReply.slice(3, 5).map(wireUint64).join(',') !== '7,8')
    throw new Error('operation uint64 tuple fields drifted');

const canonical = canonicalRequestText({
    extensionInstance: 'ab'.repeat(16),
    expectedSourceGeneration: 7n,
    expectedStateGeneration: 8n,
    expectedRole: 0,
    targetRole: 1,
});
const digest = GLib.compute_checksum_for_string(
    GLib.ChecksumType.SHA256, canonical, -1);
if (digest !== 'c099025ad75ee5ca9f6986556130eabc' +
    '71aaae96e449b20cb2df09517bde0e00')
    throw new Error('canonical request SHA-256 drifted');

for (const invalid of [
    -1, 1.5, Number.MAX_SAFE_INTEGER + 1, -1n,
    18_446_744_073_709_551_616n,
]) {
    let rejected = false;
    try {
        wireUint64(invalid);
    } catch (_error) {
        rejected = true;
    }
    if (!rejected)
        throw new Error(`unsafe uint64 accepted: ${invalid}`);
}

console.log('test_gjs_uint64_variant: OK');
