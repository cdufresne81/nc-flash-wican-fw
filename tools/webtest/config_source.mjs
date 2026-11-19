// Read the config.json contract out of main/config_server.c, so the web tests can check the page
// against what the firmware actually does instead of against a hand-copied key list.
//
// Three facts, all extracted from source text:
//   - which keys config_server_parse_cfg_into() REQUIRES (a missing one fails the parse),
//   - which keys it reads at all,
//   - the factory default device_config_default[], as the JSON string the firmware writes.
//
// This is regex over C, so it is only trustworthy while it finds something. Callers assert a
// plausible minimum on every set it returns; a refactor that defeats a pattern (a renamed
// function, cJSON_GetObjectItemCaseSensitive, a default built at run time) then fails the suite by
// name instead of quietly checking an empty set.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const CONFIG_SERVER_C = join(REPO, 'main', 'config_server.c');
// Headers whose string #defines can appear in device_config_default (SYNC_SYS_TIME_DEFAULT_TZ).
const MACRO_SOURCES = [CONFIG_SERVER_C, join(REPO, 'main', 'sync_sys_time.h'),
                       join(REPO, 'main', 'config_server.h')];

// C translation phases 2 and 3, the parts that matter here: join backslash-newline line splices
// (device_config_default spans lines that way), then blank out comments without touching string
// literals. config_server.c keeps two retired defaults in // comments directly under the live one,
// so a pattern run over the raw text would happily read a dead default.
export function cleanC(src) {
    src = src.replace(/\\\r?\n/g, '');
    let out = '';
    for (let i = 0; i < src.length;) {
        const c = src[i], d = src[i + 1];
        if (c === '/' && d === '*') {
            const e = src.indexOf('*/', i + 2);
            const end = e < 0 ? src.length : e + 2;
            out += src.slice(i, end).replace(/[^\n]/g, ' ');
            i = end;
        } else if (c === '/' && d === '/') {
            while (i < src.length && src[i] !== '\n') i++;
        } else if (c === '"' || c === "'") {
            let j = i + 1;
            while (j < src.length && src[j] !== c) j += (src[j] === '\\') ? 2 : 1;
            out += src.slice(i, j + 1);
            i = j + 1;
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

function decodeCString(body) {
    const simple = { n: '\n', t: '\t', r: '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'", '?': '?' };
    return body.replace(/\\(x[0-9a-fA-F]+|[0-7]{1,3}|.)/g, (_, e) => {
        if (e[0] === 'x') return String.fromCharCode(parseInt(e.slice(1), 16));
        if (/^[0-7]+$/.test(e)) return String.fromCharCode(parseInt(e, 8));
        if (e in simple) return simple[e];
        throw new Error(`unsupported C escape \\${e}`);
    });
}

// Index of the brace matching the "{" at `open`, skipping string and char literals.
function matchBrace(src, open) {
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        const c = src[i];
        if (c === '"' || c === "'") {
            let j = i + 1;
            while (j < src.length && src[j] !== c) j += (src[j] === '\\') ? 2 : 1;
            i = j;
        } else if (c === '{') depth++;
        else if (c === '}' && --depth === 0) return i;
    }
    throw new Error('unbalanced braces');
}

function stringMacros() {
    const macros = new Map();
    for (const f of MACRO_SOURCES) {
        for (const m of cleanC(readFileSync(f, 'utf8')).matchAll(/^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+"((?:[^"\\]|\\.)*)"[ \t]*$/gm)) {
            macros.set(m[1], decodeCString(m[2]));
        }
    }
    return macros;
}

/** device_config_default[] as the exact text the firmware writes to config.json. */
export function factoryDefaultText(src = readFileSync(CONFIG_SERVER_C, 'utf8')) {
    const clean = cleanC(src);
    const decls = [...clean.matchAll(/\bdevice_config_default\s*\[\s*\]\s*=/g)];
    if (decls.length !== 1) {
        throw new Error(`expected one live device_config_default[] definition, found ${decls.length}`);
    }
    // The initializer is a run of adjacent string literals and string macros, ending at ";".
    const macros = stringMacros();
    let i = decls[0].index + decls[0][0].length, text = '';
    for (;;) {
        while (/\s/.test(clean[i])) i++;
        if (clean[i] === ';') return text;
        if (clean[i] === '"') {
            let j = i + 1;
            while (clean[j] !== '"') j += (clean[j] === '\\') ? 2 : 1;
            text += decodeCString(clean.slice(i + 1, j));
            i = j + 1;
            continue;
        }
        const id = /^\w+/.exec(clean.slice(i));
        if (id && macros.has(id[0])) {
            text += macros.get(id[0]);
            i += id[0].length;
            continue;
        }
        throw new Error(`device_config_default: cannot evaluate "${clean.slice(i, i + 40)}"`);
    }
}

// Split an if-condition on its top-level "||".
function topLevelDisjuncts(cond) {
    const parts = [];
    let depth = 0, start = 0;
    for (let i = 0; i < cond.length; i++) {
        if (cond[i] === '(') depth++;
        else if (cond[i] === ')') depth--;
        else if (depth === 0 && cond.startsWith('||', i)) { parts.push(cond.slice(start, i)); start = i + 2; i++; }
    }
    parts.push(cond.slice(start));
    return parts.map(p => p.trim());
}

// If `text` starts with `if (cond) <stmt-or-block>`, return { cond, body }.
function leadingIf(text) {
    const m = /^\s*if\s*\(/.exec(text);
    if (!m) return null;
    let depth = 1, i = m[0].length;
    for (; i < text.length && depth; i++) {
        if (text[i] === '(') depth++;
        else if (text[i] === ')') depth--;
    }
    const cond = text.slice(m[0].length, i - 1);
    const rest = text.slice(i);
    const ws = /^\s*/.exec(rest)[0].length;
    if (rest[ws] === '{') return { cond, body: rest.slice(ws, ws + matchBrace(rest.slice(ws), 0) + 1) };
    return { cond, body: rest.slice(0, rest.indexOf(';') + 1) };
}

/**
 * Classify every top-level key config_server_parse_cfg_into() looks up.
 *
 * The parser is a flat run of `var = cJSON_GetObjectItem(root, "K");` lookups, each followed by its
 * own handling up to the next lookup. Each key's handling (its "segment") is classified as:
 *   required    -- the statement right after the lookup is `if (...)` with `var == 0`, `var == NULL`
 *                  or `!var` as one of its top-level || terms, and its body does `goto config_error`.
 *                  An absent key fails the whole parse.
 *   conditional -- the segment can `goto config_error`, but not simply because the key is absent
 *                  (e.g. ap_ssid fails only while ap_ssid_en is "enable"). Kept separate so a new one
 *                  has to be looked at by a human rather than silently counted as optional.
 *   optional    -- no `goto config_error` in the segment: absent or garbage falls back to a default
 *                  (the MIGRATION-SAFE keys).
 * Required keys also fail when present but not a string (`key->valuestring == NULL`), and some
 * when the string is the wrong length. `bounds` maps each required key that has a length check to
 * { min, max } in bytes (strlen), read from the `strlen(var->valuestring) <op> N` terms of its
 * segment's `goto config_error` conditions: `== 0` gives min 1, `< N` min N, `> N` max N.
 *
 * Every lookup on `root` must be in the recognised `var = cJSON_GetObjectItem(root, "K");` form.
 * Any other shape (cJSON_GetObjectItemCaseSensitive, a lookup inside an if, cJSON_HasObjectItem)
 * would be invisible to the classification -- a new required key, or a revived retired one, would
 * pass every test -- so it throws instead.
 */
export function parserKeys(src = readFileSync(CONFIG_SERVER_C, 'utf8')) {
    const clean = cleanC(src);
    const defs = [...clean.matchAll(/\bconfig_server_parse_cfg_into\s*\([^)]*\)\s*\{/g)];
    if (defs.length !== 1) {
        throw new Error(`expected one config_server_parse_cfg_into() definition, found ${defs.length}`);
    }
    const open = defs[0].index + defs[0][0].length - 1;
    const body = clean.slice(open, matchBrace(clean, open) + 1);

    const lookups = [...body.matchAll(/(\w+)\s*=\s*cJSON_GetObjectItem\s*\(\s*root\s*,\s*"([^"]+)"\s*\)\s*;/g)];
    const anyLookup = [...body.matchAll(/cJSON_(?:GetObjectItem\w*|HasObjectItem)\s*\(\s*root\b/g)].length;
    if (anyLookup !== lookups.length) {
        throw new Error(`config_server_parse_cfg_into() has ${anyLookup} lookups on root but only ` +
            `${lookups.length} are in the recognised \`var = cJSON_GetObjectItem(root, "K");\` form; ` +
            `rewrite the new one in that form or teach parserKeys() its shape`);
    }
    const reads = new Set(), required = new Set(), conditional = new Set(), optional = new Set();
    const bounds = new Map();
    lookups.forEach((m, n) => {
        const [, v, key] = m;
        const from = m.index + m[0].length;
        const segment = body.slice(from, n + 1 < lookups.length ? lookups[n + 1].index : body.length);
        reads.add(key);
        const lead = leadingIf(segment);
        const absentTest = new RegExp(`^(?:${v}\\s*==\\s*(?:0|NULL)|!\\s*${v})$`);
        if (lead && /\bgoto\s+config_error\s*;/.test(lead.body) &&
            topLevelDisjuncts(lead.cond).some(t => absentTest.test(t))) {
            required.add(key);
            const b = lengthBounds(segment, v);
            if (b) bounds.set(key, b);
        } else if (/\bgoto\s+config_error\s*;/.test(segment)) {
            conditional.add(key);
        } else {
            optional.add(key);
        }
    });
    return { reads, required, conditional, optional, bounds };
}

// The strlen() limits a segment enforces on `v`, from the conditions of its `goto config_error`
// ifs only (a strlen in a log line or a fallback branch is not a parse failure).
function lengthBounds(segment, v) {
    let min = 0, max = Infinity, found = false;
    const term = new RegExp(`^strlen\\s*\\(\\s*${v}\\s*->\\s*valuestring\\s*\\)\\s*(==|<|>)\\s*(\\d+)$`);
    for (const m of segment.matchAll(/\bif\s*\(/g)) {
        const lead = leadingIf(segment.slice(m.index));
        if (!lead || !/\bgoto\s+config_error\s*;/.test(lead.body)) continue;
        for (const t of topLevelDisjuncts(lead.cond)) {
            const x = term.exec(t.replace(/^\((.*)\)$/, '$1').trim());
            if (!x) continue;
            const n = Number(x[2]);
            found = true;
            if (x[1] === '==' && n === 0) min = Math.max(min, 1);
            else if (x[1] === '<') min = Math.max(min, n);
            else if (x[1] === '>') max = Math.min(max, n);
            else throw new Error(`unhandled length check on ${v}: ${t}`);
        }
    }
    return found ? { min, max } : null;
}

/** A numeric #define from config_server.c (AP_SSID_MIN_LEN and friends). */
export function numericDefine(name, src = readFileSync(CONFIG_SERVER_C, 'utf8')) {
    const m = new RegExp(`^[ \\t]*#[ \\t]*define[ \\t]+${name}[ \\t]+\\(?(\\d+)\\)?[ \\t]*$`, 'm').exec(cleanC(src));
    if (!m) throw new Error(`#define ${name} not found in config_server.c`);
    return Number(m[1]);
}
