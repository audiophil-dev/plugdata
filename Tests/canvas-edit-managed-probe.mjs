import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, mkdirSync, readFileSync, realpathSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createConnection, createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, args) => {
    if (index % 2 === 0) pairs.push([value, args[index + 1]]);
    return pairs;
}, []));
assert.deepEqual(Object.keys(options).sort(), ['--binary', '--output']);
const binary = realpathSync(resolve(options['--binary']));
assert(binary.endsWith('/Plugins/Standalone-managed/plugdata.app/Contents/MacOS/plugdata'));
const output = resolve(options['--output']);
const sibling = resolve(import.meta.dirname, '../../pd-mcp/pd');
const directory = mkdtempSync(join(tmpdir(), 'canvas-edit-managed-'));
const wait = (ms) => new Promise((done) => setTimeout(done, ms));
const evidence = { binary, flavor: 'ENABLE_TESTING=0', events: [], checks: [], cleanup: {}, status: 'failed' };
let child;
let listener;
let requestId = 100;
let frameBuffer = '';
const pending = new Map();
let relayFailure;

function check(condition, description) {
    assert(condition, description);
    evidence.checks.push(description);
}

async function freePort() {
    const server = createServer();
    await new Promise((done, fail) => server.listen(0, '127.0.0.1', done).once('error', fail));
    const port = server.address().port;
    await new Promise((done) => server.close(done));
    return port;
}

async function send(port, frame) {
    await new Promise((done, fail) => {
        const socket = createConnection({ host: '127.0.0.1', port }, () => socket.end(frame));
        socket.once('error', fail);
        socket.once('close', done);
    });
}

function onFrame(frame) {
    const encoded = frame.trim().replace(/^response /, '');
    if (!encoded) return;
    let reply;
    try {
        assert(Buffer.byteLength(encoded) <= 61440, 'encoded reply exceeds 60 KiB');
        reply = JSON.parse(Buffer.from(encoded, 'base64').toString('utf8'));
    } catch (error) {
        relayFailure = error;
        for (const entry of pending.values()) {
            clearTimeout(entry.timer);
            entry.reject(error);
        }
        pending.clear();
        return;
    }
    const entry = pending.get(reply.request_id);
    if (!entry) {
        evidence.events.push({ late_reply: reply, encoded_bytes: Buffer.byteLength(encoded) });
        return;
    }
    clearTimeout(entry.timer);
    pending.delete(reply.request_id);
    entry.resolve({ reply, encodedBytes: Buffer.byteLength(encoded) });
}

async function request(editPort, operation, fields, timeoutMs = 10000) {
    if (relayFailure) throw relayFailure;
    const envelope = { version: 1, request_id: ++requestId, operation, ...fields };
    const encoded = Buffer.from(JSON.stringify(envelope)).toString('base64');
    let timer;
    const response = new Promise((resolve, reject) => {
        timer = setTimeout(() => {
            pending.delete(envelope.request_id);
            reject(new Error(`DebugTimeout ${envelope.request_id}`));
        }, timeoutMs);
        pending.set(envelope.request_id, { timer, resolve, reject });
    });
    response.catch(() => {});
    try {
        await send(editPort, `pd_mcp_target __pd_mcp_debug request ${encoded};\n`);
        const result = await response;
        check(result.reply.request_id === envelope.request_id, `correlated ${operation} ${envelope.request_id}`);
        evidence.events.push({ request: envelope, ...result });
        return result.reply;
    } catch (error) {
        clearTimeout(timer);
        pending.delete(envelope.request_id);
        evidence.events.push({ request: envelope, outcome: 'unknown', error: String(error) });
        throw error;
    }
}

async function read(editPort, generation, path = [], expectedRevision) {
    const records = [];
    let cursor;
    let revision = expectedRevision;
    let largest = 0;
    let pages = 0;
    do {
        const reply = await request(editPort, 'read_canvas', {
            generation, canvas_path: path,
            ...(revision ? { expected_revision: revision } : {}),
            ...(cursor ? { cursor } : {}),
        });
        check(reply.ok === true, `read page ${pages} succeeds`);
        if (revision) check(reply.data.revision === revision, 'page revision matches');
        revision = reply.data.revision;
        check(Array.isArray(reply.data.records) && reply.data.records.length > 0, 'read page makes progress');
        records.push(...reply.data.records);
        cursor = reply.data.next_cursor;
        const encodedBytes = evidence.events.at(-1).encodedBytes;
        largest = Math.max(largest, encodedBytes);
        check(encodedBytes <= 61440, 'binary relay page within 60 KiB');
        pages++;
        check(pages < 30, 'bounded page count');
    } while (cursor);
    return { revision, records, largest, pages };
}

function object(snapshot, path, ordinal) {
    return snapshot.records.find((record) => record.kind === 'object'
        && JSON.stringify(record.canvas_path) === JSON.stringify(path) && record.ordinal === ordinal);
}

function connection(snapshot, path, source, outlet, target, inlet) {
    return snapshot.records.some((record) => record.kind === 'connection'
        && JSON.stringify(record.canvas_path) === JSON.stringify(path)
        && record.source === source && record.outlet === outlet && record.target === target && record.inlet === inlet);
}

async function until(action, description) {
    for (let attempt = 0; attempt < 50; attempt++) {
        if (await action()) return;
        await wait(200);
    }
    throw new Error(`Timed out waiting for ${description}`);
}

try {
    const sourceWrapper = readFileSync(join(sibling, 'pd_mcp_target_session_plugdata.pd'), 'utf8');
    const sourceBridge = readFileSync(join(sibling, 'pd_mcp_bridge.pd'), 'utf8');
    check(sourceWrapper.includes('netreceive -f 3013;') && sourceBridge.includes('netreceive 3011;'), 'sibling wrapper ports match expected source');
    const bridgePort = await freePort();
    let editPort = await freePort();
    while (editPort === bridgePort) editPort = await freePort();
    check(![3011, 3013].includes(editPort) && ![3011, 3013].includes(bridgePort), 'isolated ports exclude foreign listeners');
    writeFileSync(join(directory, 'pd_mcp_target_session_plugdata.pd'), sourceWrapper.replace('netreceive -f 3013;', `netreceive -f ${editPort};`));
    writeFileSync(join(directory, 'pd_mcp_bridge.pd'), sourceBridge.replace('netreceive 3011;', `netreceive ${bridgePort};`));
    const home = join(directory, 'home');
    mkdirSync(join(home, 'Documents', 'plugdata'), { recursive: true });
    listener = createServer((socket) => socket.on('data', (chunk) => {
        frameBuffer += chunk.toString('utf8');
        const frames = frameBuffer.split(';');
        frameBuffer = frames.pop();
        for (const frame of frames) onFrame(frame);
    }));
    await new Promise((done, fail) => listener.listen(0, '127.0.0.1', done).once('error', fail));
    const replyPort = listener.address().port;
    const args = ['-stderr', '-open', join(directory, 'pd_mcp_target_session_plugdata.pd'), '-send', `pd-mcp-edit-port ${editPort}`];
    evidence.process = { pids: [], args, home, bridgePort, editPort, replyPort };
    async function startOwnedChild() {
        child = spawn(binary, args, { env: { ...process.env, PD_MCP_BINARY: binary, HOME: home }, stdio: ['ignore', 'pipe', 'pipe'] });
        evidence.process.pids.push(child.pid);
        let stderr = '';
        child.stderr.on('data', (chunk) => { stderr = (stderr + chunk.toString()).slice(-6000); });
        await until(() => {
            if (child.exitCode !== null) throw new Error(`Managed binary exited: ${child.exitCode}\n${stderr}`);
            try {
                return [editPort, bridgePort].every((port) => {
                    const owners = execFileSync('lsof', ['-nP', `-iTCP:${port}`, '-sTCP:LISTEN', '-t'], { encoding: 'utf8' }).trim().split(/\s+/);
                    return owners.length === 1 && owners[0] === String(child.pid);
                });
            } catch { return false; }
        }, 'owned relay listeners');
        check(true, `both isolated listeners owned by PID ${child.pid}`);
        await send(editPort, `pd_mcp_debug_port ${replyPort};\n`);
        await wait(300);
    }
    await startOwnedChild();
    const handshake = await request(editPort, 'set_generation', { generation: 'managed-probe', root_receiver: 'absent-managed-probe' });
    check(handshake.ok === false && handshake.error.code === 'CanvasNotFound', 'real receiver answered isolated handshake');

    const fixture = join(directory, 'managed-fixture.pd');
    const patch = '#N canvas 100 100 800 500 12;\n'
        + '#X obj 20 20 f 73;\n#X obj 20 60 print state-probe;\n'
        + '#N canvas 0 0 300 200 twin 0;\n#X obj 20 20 t b b;\n#X obj 20 50 print first;\n#X obj 120 50 print preserved;\n#X connect 0 0 1 0;\n#X connect 0 1 2 0;\n#X restore 180 80 pd twin;\n'
        + '#N canvas 0 0 300 200 twin 0;\n#X obj 20 20 print second;\n#X restore 350 80 pd twin;\n'
        + '#X obj 200 20 t b b;\n#X obj 350 20 print root;\n'
        + '#X obj 20 130 r pd;\n#X obj 20 160 route dsp;\n#X obj 20 190 print dsp-probe;\n'
        + '#X connect 0 0 1 0;\n#X connect 4 0 5 0;\n#X connect 6 0 7 0;\n#X connect 7 0 8 0;\n';
    writeFileSync(fixture, patch);
    const generation = 'managed-fixture-generation';
    const rootReceiver = `pd-${basename(fixture)}`;
    const pd = (receiver, message) => send(editPort, `pd_mcp_target ${receiver} ${message};\n`);
    await pd('pd', `open ${basename(fixture)} ${dirname(fixture)}`);
    let bound;
    await until(async () => {
        bound = await request(editPort, 'set_generation', { generation, root_receiver: rootReceiver });
        return bound.ok;
    }, 'fixture canvas receiver registration');
    let baseline = await read(editPort, generation);
    check(baseline.records.filter((record) => record.kind === 'canvas' && record.name === 'twin').length === 2, 'same-named sibling canvases are distinct');
    const sendObject = (ordinal, selector, atoms = []) => request(editPort, 'send_object', {
        generation, canvas_path: [], object_ordinal: ordinal, selector, atoms,
    });
    await pd('pd', 'dsp 1');
    await sendObject(0, 'float', [321]);
    await sendObject(0, 'bang');
    const consoleReply = () => request(editPort, 'get_console', { include_history: true, max_entries: 200 });
    let consoleBefore = await consoleReply();
    check(consoleBefore.ok && consoleBefore.data.entries.some((entry) => entry.text.includes('state-probe: 321')), '[f] holds 321 before edits');
    check(consoleBefore.data.entries.some((entry) => entry.text.includes('dsp-probe: 1')), 'DSP is on before edits');

    const edits = [
        { op: 'move', canvas_path: [2], target: 0, x: 45, y: 50 },
        { op: 'remove', canvas_path: [2], target: 1 },
        { op: 'add', canvas_path: [], ref: 'new', type: 'object', x: 450, y: 45, text: 'print new' },
        { op: 'connect', canvas_path: [], source: 4, outlet: 1, target: { ref: 'new' }, inlet: 0 },
    ];
    const applied = await request(editPort, 'edit_canvas', { generation, expected_revision: baseline.revision, edits });
    check(applied.ok && applied.data.status === 'applied', 'in-place managed edit applied');
    const after = await read(editPort, generation);
    check(after.revision === applied.data.revision && after.revision !== baseline.revision, 'applied revision matches live readback');
    check(object(after, [2], 0)?.x === 45 && object(after, [2], 0)?.y === 50, 'only first twin moved');
    check(object(after, [3], 0)?.text === 'print second', 'second twin intact');
    check(connection(after, [2], 0, 1, 1, 0), 'survivor first-twin cord intact');
    check(!connection(after, [2], 0, 0, 1, 0), 'removed first-twin incident cord gone');
    check(connection(after, [], 4, 1, applied.data.mappings.refs.new.ordinal, 0), 'new root cord present');
    check(connection(after, [], 0, 0, 1, 0) && connection(after, [], 4, 0, 5, 0), 'unrelated root cords intact');
    await sendObject(0, 'bang');
    const consoleAfter = await consoleReply();
    check(consoleAfter.data.entries.some((entry) => entry.text.includes('state-probe: 321') && entry.id > consoleBefore.data.cursor), '[f] value survives live edit');
    check(!consoleAfter.data.entries.some((entry) => entry.text.includes('dsp-probe: 0') && entry.id > consoleBefore.data.cursor), 'edit did not turn DSP off');
    await pd('pd', 'dsp 1');
    const dspAfter = await consoleReply();
    check(dspAfter.data.entries.some((entry) => entry.text.includes('dsp-probe: 1') && entry.id > consoleAfter.data.cursor), 'DSP reports on after live edit');

    for (const [label, fields, code] of [
        ['invalid port', { generation, expected_revision: after.revision, edits: [{ op: 'connect', canvas_path: [], source: 4, outlet: 99, target: 5, inlet: 0 }] }, 'InvalidPort'],
        ['stale generation', { generation: 'wrong-generation', expected_revision: after.revision, edits: [{ op: 'move', canvas_path: [], target: 0, x: 25, y: 25 }] }, 'StaleGeneration'],
        ['malformed', { generation, expected_revision: after.revision, edits: [{ op: 'move', canvas_path: [], target: 0, x: 25, y: 25, extra: true }] }, 'InvalidRequest'],
    ]) {
        const rejected = await request(editPort, 'edit_canvas', fields);
        check(!rejected.ok && rejected.error.code === code, `${label} rejected as ${code}`);
        const unchanged = await read(editPort, generation);
        check(JSON.stringify(unchanged.records) === JSON.stringify(after.records), `${label} leaves structure unchanged`);
    }

    const stale = after.revision;
    await pd(rootReceiver, 'obj 550 100 print external');
    const external = await read(editPort, generation);
    check(external.revision !== stale && external.records.some((record) => record.text === 'print external'), 'external Pd-side edit changes root revision');
    const staleReply = await request(editPort, 'edit_canvas', { generation, expected_revision: stale, edits: [{ op: 'move', canvas_path: [3], target: 0, x: 100, y: 100 }] });
    check(!staleReply.ok && staleReply.error.code === 'StaleRevision', 'external change rejects stale edit');
    check(JSON.stringify((await read(editPort, generation)).records) === JSON.stringify(external.records), 'stale edit does not mutate live structure');

    const timeoutBefore = object(external, [3], 0);
    check(timeoutBefore?.x === 20 && timeoutBefore.y === 20, 'timeout target starts at known coordinates');
    const timeoutRequestId = requestId + 1;
    let timedOut = false;
    try {
        await request(editPort, 'edit_canvas', { generation, expected_revision: external.revision, edits: [{ op: 'move', canvas_path: [3], target: 0, x: 95, y: 95 }] }, 1);
    } catch (error) {
        check(String(error).includes('DebugTimeout'), 'edit timeout is recorded as unknown, not retried');
        timedOut = true;
    }
    check(timedOut, 'one deliberately timed-out edit was not resent');
    await wait(300);
    const resolved = await read(editPort, generation);
    const timeoutAfter = object(resolved, [3], 0);
    const appliedAfterTimeout = timeoutAfter?.x === 95 && timeoutAfter.y === 95;
    check(timeoutAfter?.text === 'print second' && (appliedAfterTimeout || (timeoutAfter.x === 20 && timeoutAfter.y === 20)),
        'timeout readback distinguishes applied from unchanged coordinates');
    check(evidence.events.filter((event) => event.request?.request_id === timeoutRequestId).length === 1,
        'timed-out request was sent only once');
    evidence.timeout = {
        requestId: timeoutRequestId, outcome: appliedAfterTimeout ? 'applied' : 'unchanged',
        before: { x: timeoutBefore.x, y: timeoutBefore.y, revision: external.revision },
        after: { x: timeoutAfter.x, y: timeoutAfter.y, revision: resolved.revision },
    };

    await pd(rootReceiver, 'menusave');
    await until(() => readFileSync(fixture, 'utf8').includes('#X obj 45 50 t b b;'), 'native save of edited patch');
    const saved = readFileSync(fixture, 'utf8');
    check(saved.includes('#X obj 20 20 f 73;') && saved.includes('print external'), 'saved patch contains state object and external edit');
    check(saved.includes('#X connect 0 1 1 0;') && saved.includes('#X connect 4 1 9 0;'), 'saved patch contains surviving and new cords');
    evidence.saved = { content: saved, sha256: createHash('sha256').update(saved).digest('hex'), bytes: statSync(fixture).size };
    await pd(rootReceiver, 'menuclose 1');
    await until(async () => {
        const result = await request(editPort, 'set_generation', { generation: 'after-close', root_receiver: rootReceiver });
        return !result.ok && result.error.code === 'CanvasNotFound';
    }, 'document receiver release');
    const closingPid = child.pid;
    child.kill('SIGTERM');
    await until(() => child.exitCode !== null || child.signalCode !== null, 'owned process close');
    evidence.cleanup.closedPid = closingPid;
    await startOwnedChild();
    await pd('pd', `open ${basename(fixture)} ${dirname(fixture)}`);
    const reopenedGeneration = 'reopened-fixture-generation';
    await until(async () => (await request(editPort, 'set_generation', { generation: reopenedGeneration, root_receiver: rootReceiver })).ok, 'canvas reopen');
    const reopened = await read(editPort, reopenedGeneration);
    check(JSON.stringify(reopened.records) === JSON.stringify(resolved.records), 'saved and reopened structure matches live model');
    evidence.reopened = { revision: reopened.revision, records: reopened.records };

    const large = join(directory, 'large-relay.pd');
    let largePatch = '#N canvas 100 100 400 300 12;\n#X msg 20 20 '
        + Array.from({ length: 320 }, (_, index) => 'q'.repeat(100) + index).join(' ') + ';\n';
    for (let index = 0; index < 440; index++)
        largePatch += `#X text 10 ${120 + index * 18} snapshot-${index}-${'x'.repeat(96)};\n`;
    writeFileSync(large, largePatch);
    await pd('pd', `open ${basename(large)} ${dirname(large)}`);
    const largeReceiver = `pd-${basename(large)}`;
    const largeGeneration = 'large-fixture-generation';
    await until(async () => (await request(editPort, 'set_generation', { generation: largeGeneration, root_receiver: largeReceiver })).ok, 'large canvas registration');
    const pages = await read(editPort, largeGeneration);
    check(pages.pages > 1 && pages.largest >= 56 * 1024, 'near-60-KiB binary reply survives intact');
    check(pages.records.length === 442, 'all 442 paged records reassembled');
    const largeRecord = pages.records.find((record) => record.kind === 'object' && record.ordinal === 0);
    check(largeRecord.text.includes('q'.repeat(100) + '319') && largeRecord.text.length > 32000, 'large single record survives without truncation');
    const finalComment = pages.records.at(-1);
    check(finalComment.kind === 'object' && finalComment.type === 'comment' && finalComment.ordinal === 440
        && JSON.stringify(finalComment.canvas_path) === '[]' && finalComment.text === `snapshot-439-${'x'.repeat(96)}`
        && finalComment.x === 10 && finalComment.y === 8022, 'last paged comment has expected text and root address');
    evidence.large = { pages: pages.pages, largestEncodedBytes: pages.largest, recordBytes: Buffer.byteLength(JSON.stringify(largeRecord)), records: pages.records.length };
    evidence.status = 'passed';
    console.log(`managed applied=${applied.request_id} stale=${staleReply.request_id} pages=${pages.pages} largest=${pages.largest} pid=${child.pid}`);
} catch (error) {
    evidence.failure = { message: String(error), stack: error.stack };
    console.error(error);
} finally {
    for (const entry of pending.values()) clearTimeout(entry.timer);
    if (child) {
        child.kill('SIGTERM');
        for (let attempt = 0; attempt < 30 && child.exitCode === null && child.signalCode === null; attempt++) await wait(100);
        if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
        evidence.cleanup.child = { pid: child.pid, exitCode: child.exitCode, signalCode: child.signalCode };
    }
    if (listener) await new Promise((done) => listener.close(done));
    rmSync(directory, { recursive: true, force: true });
    evidence.cleanup.tempPath = directory;
    evidence.cleanup.tempRemoved = !existsSync(directory);
    writeFileSync(output, JSON.stringify(evidence, null, 2) + '\n');
}
if (evidence.status !== 'passed') process.exitCode = 1;
