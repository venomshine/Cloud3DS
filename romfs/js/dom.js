/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 *
 * El DOM que ve el JavaScript de una página, encima de lo mínimo en C (__n, source/jsdom.c):
 * document, window, Node/Element/Text, classList, style, dataset, eventos, temporizadores,
 * fetch, XMLHttpRequest, localStorage… Lo que no hace falta para colocar la página (medidas,
 * pintar) devuelve valores razonables para que los scripts sigan.
 */
(function (G) {
'use strict';
const N = __n;
const wrappers = new Map();          // un objeto por nodo: el mismo siempre (a === b)
const listeners = new Map();          // id del nodo -> {tipo: [funciones]}
const ELEMENT = 1, TEXT = 3, COMMENT = 8, DOC = 9, FRAG = 11;

function W(h) {
	if (!h) return null;
	const id = N.id(h);
	let w = wrappers.get(id);
	if (w) return w;
	const t = N.type(h);
	w = Object.create(t === ELEMENT ? Element.prototype : t === TEXT ? Text.prototype : t === COMMENT ? Comment.prototype :
		t === DOC ? Document.prototype : t === FRAG ? DocumentFragment.prototype : Node.prototype);
	w._h = h;
	wrappers.set(id, w);
	return w;
}
function H(w) { return w && w._h ? w._h : null; }
function nodeOf(x) { return typeof x === 'string' ? document.createTextNode(x) : x; }
/* un <script> que la página mete con appendChild/insertBefore se ejecuta (como en un navegador; los de
 * innerHTML no). Los externos, en cuanto se pueda, y luego su evento load (o error) */
const ran = new Set();
function jsType(el) { const t = (el.getAttribute('type') || '').toLowerCase(); return t === 'module' || (!el.hasAttribute('nomodule') && (!t || /^(text|application)\/(x-)?(java|ecma)script$/.test(t))); }
function inserted(node) {
	if (!node || !node._h) return;
	if (ceDefs.size && node.isConnected) ceSubtree(node);
	const scripts = node.nodeType === ELEMENT && node.localName === 'script' ? [node] : node.nodeType === ELEMENT || node.nodeType === FRAG ? qsa(node, 'script') : [];
	for (const sc of scripts) {
		const id = N.id(sc._h);
		if (ran.has(id) || !jsType(sc) || !sc.isConnected) continue;
		ran.add(id);
		const src = sc.getAttribute('src'), mod = (sc.getAttribute('type') || '').toLowerCase() === 'module';
		if (src) setTimeout(() => {
			const prev = G.__script; G.__script = mod ? null : sc;
			const ok = mod ? N.runModule(resolveUrl(location.href, src), true) : N.runSrc(resolveUrl(location.href, src));
			G.__script = prev;
			sc.dispatchEvent(new Event(ok ? 'load' : 'error'));
		}, 0);
		else if (mod) setTimeout(() => N.runModule(sc.textContent, false), 0);
		else { const prev = G.__script; G.__script = sc; N.runCode(sc.textContent); G.__script = prev; }
	}
}

/* ---------- eventos */
const NONE = 0, CAPTURING = 1, AT_TARGET = 2, BUBBLING = 3;
class Event {
	constructor(type, init) {
		init = init || {};
		this.type = String(type); this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable; this.composed = !!init.composed;
		this.defaultPrevented = false; this._stop = false; this._stopNow = false; this.target = null; this.currentTarget = null;
		this.eventPhase = NONE; this.isTrusted = false; this.timeStamp = N.now(); this.returnValue = true; this.cancelBubble = false;
		// lo demás que traiga (detail, data, key, clientX…: CustomEvent, MessageEvent, KeyboardEvent, MouseEvent…)
		for (const k in init) if (!(k in this)) this[k] = init[k];
	}
	preventDefault() { if (this.cancelable) { this.defaultPrevented = true; this.returnValue = false; } }
	stopPropagation() { this._stop = true; this.cancelBubble = true; }
	stopImmediatePropagation() { this._stop = true; this._stopNow = true; }
	composedPath() { const p = []; for (let n = this.target; n; n = n === document ? G : n === G ? null : n.parentNode) p.push(n); return p; }
	get srcElement() { return this.target; }
	initEvent(type, bubbles, cancelable) { this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
	initCustomEvent(type, bubbles, cancelable, detail) { this.initEvent(type, bubbles, cancelable); this.detail = detail; }
}
Event.NONE = NONE; Event.CAPTURING_PHASE = CAPTURING; Event.AT_TARGET = AT_TARGET; Event.BUBBLING_PHASE = BUBBLING;
class CustomEvent extends Event { constructor(t, i) { super(t, i); if (!('detail' in this)) this.detail = null; } }
/* escuchas: por nodo (los de libdom, por su id; window y los demás objetos, en un WeakMap) y por tipo, con
 * captura, once y passive como en un navegador */
const objListeners = new WeakMap();
function lmap(t, make) {
	if (t._h) { const id = N.id(t._h); let m = listeners.get(id); if (!m && make) listeners.set(id, m = {}); return m; }
	let m = objListeners.get(t); if (!m && make) objListeners.set(t, m = {}); return m;
}
class EventTarget {
	addEventListener(type, fn, opt) {
		if (!fn) return;
		if (this == null) return G.addEventListener(type, fn, opt); // (suelta: «addEventListener(…)» es la de window)
		const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture), once = !!(opt && opt.once);
		const m = lmap(this, true), l = m[type] || (m[type] = []);
		if (l.some(x => x.fn === fn && x.capture === capture)) return;
		l.push({ fn, capture, once });
		if (opt && opt.signal && opt.signal.addEventListener) opt.signal.addEventListener('abort', () => this.removeEventListener(type, fn, opt));
	}
	removeEventListener(type, fn, opt) {
		if (this == null) return G.removeEventListener(type, fn, opt);
		const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
		const m = lmap(this, false); if (!m || !m[type]) return;
		m[type] = m[type].filter(x => !(x.fn === fn && x.capture === capture));
	}
	dispatchEvent(ev) {
		if (this == null) return G.dispatchEvent(ev);
		ev.target = this; ev._stop = ev._stopNow = false;
		// el camino: de window al objetivo (captura), el objetivo y de vuelta hacia arriba (burbuja)
		const path = [];
		for (let n = this; n; n = n === document ? G : n === G ? null : n.parentNode) path.push(n);
		const call = (node, phase) => {
			ev.currentTarget = node; ev.eventPhase = phase;
			const m = lmap(node, false);
			if (phase !== CAPTURING) {
				let on = node['on' + ev.type];
				if (typeof on !== 'function' && node._h && N.type(node._h) === ELEMENT) { // onclick="…" en el HTML
					const code = N.getA(node._h, 'on' + ev.type);
					if (code) try { on = inlineHandler(node, ev.type, code); } catch (e) { report(e); }
				}
				if (typeof on === 'function') try { if (on.call(node, ev) === false) ev.preventDefault(); } catch (e) { report(e); }
			}
			if (m && m[ev.type]) for (const x of m[ev.type].slice()) {
				if (phase === CAPTURING && !x.capture || phase === BUBBLING && x.capture) continue;
				if (x.once) m[ev.type] = m[ev.type].filter(y => y !== x);
				try { typeof x.fn === 'function' ? x.fn.call(node, ev) : x.fn.handleEvent(ev); } catch (e) { report(e); }
				if (ev._stopNow) break;
			}
		};
		for (let i = path.length - 1; i > 0 && !ev._stop; i--) call(path[i], CAPTURING);
		if (!ev._stop) call(this, AT_TARGET);
		if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) call(path[i], BUBBLING);
		ev.currentTarget = null; ev.eventPhase = NONE;
		return !ev.defaultPrevented;
	}
}
const inlineCache = new Map();
function inlineHandler(node, type, code) {
	const k = N.id(node._h) + ':' + type;
	let f = inlineCache.get(k);
	if (!f || f._code !== code) { f = new Function('event', code); f._code = code; inlineCache.set(k, f); }
	return f;
}
function report(e) { N.log('js: ' + (e && e.stack ? e.stack.split('\n').slice(0, 3).join(' | ') : e)); }

class DOMRectReadOnly {
	constructor(x = 0, y = 0, w = 0, h = 0) { this.x = x; this.y = y; this.width = w; this.height = h; }
	get top() { return Math.min(this.y, this.y + this.height); } get left() { return Math.min(this.x, this.x + this.width); }
	get right() { return Math.max(this.x, this.x + this.width); } get bottom() { return Math.max(this.y, this.y + this.height); }
	toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
	static fromRect(r) { r = r || {}; return new this(r.x, r.y, r.width, r.height); }
}
class DOMRect extends DOMRectReadOnly {}

/* ---------- colecciones: arrays de verdad (forEach, for…of, Array.from) con item(), y su clase
 * (instanceof NodeList / HTMLCollection) */
class NodeList extends Array { item(i) { return this[i] ?? null; } }
class HTMLCollection extends Array { item(i) { return this[i] ?? null; } namedItem(n) { return this.find(e => e.id === n || e.getAttribute('name') === n) || null; } }
function listOf(C, it) { const a = new C(); for (const x of it) a.push(x); return a; }

/* ---------- nodos */
class Node extends EventTarget {
	get nodeType() { return N.type(this._h); }
	get nodeName() { return N.name(this._h); }
	get parentNode() { return W(N.parent(this._h)); }
	get parentElement() { const p = this.parentNode; return p && p.nodeType === ELEMENT ? p : null; }
	get firstChild() { return W(N.first(this._h)); }
	get lastChild() { return W(N.last(this._h)); }
	get nextSibling() { return W(N.next(this._h)); }
	get previousSibling() { return W(N.prev(this._h)); }
	get childNodes() { const a = new NodeList(); for (let c = N.first(this._h); c; c = N.next(c)) a.push(W(c)); return a; }
	get ownerDocument() { return document; }
	get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === document; }
	get textContent() { return N.text(this._h); }
	set textContent(v) { N.setText(this._h, v == null ? '' : String(v)); }
	get nodeValue() { return this.nodeType === ELEMENT ? null : N.data(this._h); }
	set nodeValue(v) { if (this.nodeType !== ELEMENT) N.setData(this._h, String(v)); }
	hasChildNodes() { return !!N.first(this._h); }
	appendChild(c) { c = nodeOf(c); if (c instanceof DocumentFragment) { const ks = c.childNodes; for (const k of ks) N.append(this._h, k._h); for (const k of ks) inserted(k); return c; } N.append(this._h, c._h); inserted(c); return c; }
	insertBefore(c, ref) { c = nodeOf(c); if (c instanceof DocumentFragment) { const ks = c.childNodes; for (const k of ks) N.insert(this._h, k._h, H(ref)); for (const k of ks) inserted(k); return c; } N.insert(this._h, c._h, H(ref)); inserted(c); return c; }
	removeChild(c) { N.removeChild(this._h, c._h); if (c._ce && typeof c.disconnectedCallback === 'function') { c._conn = false; try { c.disconnectedCallback(); } catch (e) { report(e); } } return c; }
	replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
	cloneNode(deep) { return W(N.clone(this._h, !!deep)); }
	contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
	remove() { const p = this.parentNode; if (p) p.removeChild(this); }
	getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
	isEqualNode(o) { return !!o && o.nodeType === this.nodeType && (o === this || (this.nodeType === ELEMENT ? o.outerHTML === this.outerHTML : o.nodeValue === this.nodeValue)); }
	isSameNode(o) { return o === this; }
	normalize() {}
	get baseURI() { return location.href; }
	lookupNamespaceURI() { return null; }
	// (el puesto en el documento: DOCUMENT_POSITION_PRECEDING 2, FOLLOWING 4, CONTAINS 8, CONTAINED_BY 16)
	compareDocumentPosition(o) {
		if (o === this) return 0;
		if (this.contains(o)) return 20;
		if (o.contains && o.contains(this)) return 10;
		const order = []; walkAll(document, n => { if (n === this || n === o) order.push(n); return order.length === 2; });
		return order[0] === this ? 4 : order[0] === o ? 2 : 1;
	}
}
for (const [k, v] of Object.entries({ ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
	DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 })) { Node[k] = v; Node.prototype[k] = v; }
// todos los nodos en orden (también texto), hasta que fn devuelva true
function walkAll(root, fn) {
	const st = []; let h = N.first(root._h);
	while (h || st.length) {
		if (!h) { h = N.next(st.pop()); continue; }
		if (fn(W(h))) return true;
		const f = N.first(h); if (f) { st.push(h); h = f; continue; }
		h = N.next(h);
	}
	return false;
}
class CharacterData extends Node {
	get data() { return N.data(this._h); }
	set data(v) { N.setData(this._h, String(v)); }
	get length() { return this.data.length; }
	get nextElementSibling() { return nextEl(this); }
	get previousElementSibling() { return prevEl(this); }
	appendData(d) { this.data += d; } deleteData(o, n) { const s = this.data; this.data = s.slice(0, o) + s.slice(o + n); }
	insertData(o, d) { const s = this.data; this.data = s.slice(0, o) + d + s.slice(o); } substringData(o, n) { return this.data.substr(o, n); }
	replaceData(o, n, d) { const s = this.data; this.data = s.slice(0, o) + d + s.slice(o + n); }
	before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(nodeOf(n), this); }
	after(...ns) { const p = this.parentNode, nx = this.nextSibling; if (p) for (const n of ns) p.insertBefore(nodeOf(n), nx); }
	replaceWith(...ns) { this.before(...ns); this.remove(); }
}
class Text extends CharacterData { get wholeText() { return this.data; } splitText(o) { const t = document.createTextNode(this.data.slice(o)); this.data = this.data.slice(0, o); this.after(t); return t; } }
class Comment extends CharacterData {}
function nextEl(n) { for (let c = N.next(n._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
function prevEl(n) { for (let c = N.prev(n._h); c; c = N.prev(c)) if (N.type(c) === ELEMENT) return W(c); return null; }

/* --- selectores CSS (lo habitual: etiqueta, #id, .clase, [atributo…], :pseudo sencillas, combinadores) */
// selectores leídos carácter a carácter (sin expresiones regulares: más rápido y sin sorpresas)
function parseSel(s) {
	const groups = [];
	for (const part of splitTop(String(s), ',')) {
		const seq = []; let comb = null, i = 0; const t = part.trim();
		while (i < t.length) {
			let c = t[i];
			if (c === ' ' || c === '\t' || c === '\n' || c === '>' || c === '+' || c === '~') {
				let k = ' ';
				while (i < t.length && ' \t\n>+~'.indexOf(t[i]) >= 0) { if (t[i] !== ' ' && t[i] !== '\t' && t[i] !== '\n') k = t[i]; i++; }
				comb = k; continue;
			}
			let j = i, d = 0;
			while (j < t.length) { const ch = t[j]; if (ch === '[' || ch === '(') d++; else if (ch === ']' || ch === ')') d--; else if (!d && ' \t\n>+~'.indexOf(ch) >= 0) break; j++; }
			seq.push({ comb: seq.length ? (comb || ' ') : null, simple: parseSimple(t.slice(i, j)) });
			comb = null; i = j;
		}
		groups.push(seq);
	}
	return groups;
}
function splitTop(s, ch) { const out = []; let d = 0, cur = ''; for (const c of s) { if (c === '(' || c === '[') d++; else if (c === ')' || c === ']') d--; if (c === ch && !d) { out.push(cur); cur = ''; } else cur += c; } out.push(cur); return out; }
function isName(c) { return c && (c === '-' || c === '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c > '\x7f' || c === '\\'); }
function parseSimple(t) {
	const r = { tag: null, id: null, cls: [], attrs: [], pseudo: [] };
	let i = 0;
	const name = () => { let j = i; while (j < t.length && isName(t[j])) j++; const v = t.slice(i, j); i = j; return v; };
	if (t[0] === '*') i = 1; else if (isName(t[0])) r.tag = name().toLowerCase();
	while (i < t.length) {
		const c = t[i++];
		if (c === '#') r.id = name();
		else if (c === '.') r.cls.push(name());
		else if (c === '[') {
			let j = i, q = null; while (j < t.length && (t[j] !== ']' || q)) { if (t[j] === '"' || t[j] === "'") q = q === t[j] ? null : q || t[j]; j++; }
			const body = t.slice(i, j); i = j + 1;
			const m = /^\s*([\w:-]+)\s*(?:([~|^$*]?=)\s*(?:"([^"]*)"|'([^']*)'|(\S*)))?\s*$/.exec(body);
			if (m) r.attrs.push({ n: m[1].toLowerCase(), op: m[2], v: m[3] ?? m[4] ?? m[5] });
		} else if (c === ':') {
			if (t[i] === ':') i++;
			let p = name();
			if (t[i] === '(') { let j = i, d = 0; do { if (t[j] === '(') d++; else if (t[j] === ')') d--; j++; } while (j < t.length && d); p += t.slice(i, j); i = j; }
			r.pseudo.push(p);
		} else break;
	}
	return r;
}
function matchSimple(el, s) {
	if (el.nodeType !== ELEMENT) return false;
	if (s.tag && el.localName !== s.tag) return false;
	if (s.id && el.id !== s.id) return false;
	if (s.cls.length) { const c = ' ' + (el.getAttribute('class') || '').replace(/\s+/g, ' ') + ' '; for (const k of s.cls) if (c.indexOf(' ' + k + ' ') < 0) return false; }
	for (const a of s.attrs) {
		const v = el.getAttribute(a.n); if (v == null) return false;
		if (!a.op) continue;
		const w = a.v || '';
		if (a.op === '=' && v !== w) return false;
		if (a.op === '~=' && (' ' + v + ' ').indexOf(' ' + w + ' ') < 0) return false;
		if (a.op === '^=' && !v.startsWith(w)) return false;
		if (a.op === '$=' && !v.endsWith(w)) return false;
		if (a.op === '*=' && v.indexOf(w) < 0) return false;
		if (a.op === '|=' && v !== w && !v.startsWith(w + '-')) return false;
	}
	for (const p of s.pseudo) {
		if (p === 'first-child' && el.previousElementSibling) return false;
		if (p === 'last-child' && el.nextElementSibling) return false;
		if (p.startsWith('not(') && el.matches(p.slice(4, -1))) return false;
		if (p === 'checked' && !el.hasAttribute('checked')) return false;
		if (p === 'disabled' && !el.hasAttribute('disabled')) return false;
	}
	return true;
}
function matchSeq(el, seq, i) {
	if (!matchSimple(el, seq[i].simple)) return false;
	if (i === 0) return true;
	const c = seq[i].comb;
	if (c === '>') { const p = el.parentElement; return !!p && matchSeq(p, seq, i - 1); }
	if (c === '+') { const p = el.previousElementSibling; return !!p && matchSeq(p, seq, i - 1); }
	if (c === '~') { for (let p = el.previousElementSibling; p; p = p.previousElementSibling) if (matchSeq(p, seq, i - 1)) return true; return false; }
	for (let p = el.parentElement; p; p = p.parentElement) if (matchSeq(p, seq, i - 1)) return true;
	return false;
}
const selCache = new Map();
function compiled(sel) { let c = selCache.get(sel); if (!c) { c = parseSel(sel); selCache.set(sel, c); } return c; }
function matches(el, sel) { for (const seq of compiled(sel)) if (seq.length && matchSeq(el, seq, seq.length - 1)) return true; return false; }
function walkEls(root, fn) {
	const st = []; let h = N.first(root._h);
	while (h || st.length) {
		if (!h) { h = N.next(st.pop()); continue; }
		if (N.type(h) === ELEMENT) { if (fn(W(h))) return true; const f = N.first(h); if (f) { st.push(h); h = f; continue; } }
		h = N.next(h);
	}
	return false;
}
// lo sencillo (#id, etiqueta, .clases y su mezcla, sin combinadores ni [atributos] ni :pseudo), a la búsqueda nativa
const fastCache = new Map();
function fastSel(sel) {
	let f = fastCache.get(sel);
	if (f !== undefined) return f;
	f = null;
	const g = compiled(sel);
	if (g.length === 1 && g[0].length === 1) { const x = g[0][0].simple; if (!x.attrs.length && !x.pseudo.length && x.cls.length <= 8 && !(x.id || '').includes('\\') && !x.cls.some(c => c.includes('\\'))) f = x; }
	fastCache.set(sel, f);
	return f;
}
function qsa(root, sel) {
	const f = root && root._h && fastSel(sel);
	if (f) { const out = new NodeList(); for (const h of N.find(root._h, f.tag || '', f.id || '', f.cls, false) || []) out.push(W(h)); return out; }
	const out = new NodeList(); walkEls(root, e => { if (matches(e, sel)) out.push(e); }); return out;
}
function qs(root, sel) {
	const f = root && root._h && fastSel(sel);
	if (f) return W(N.find(root._h, f.tag || '', f.id || '', f.cls, true));
	let r = null; walkEls(root, e => { if (matches(e, sel)) { r = e; return true; } }); return r;
}

/* --- clases, estilo, data-* */
class DOMTokenList {
	constructor(el) { this._el = el; }
	_get() { return (this._el.getAttribute('class') || '').split(/\s+/).filter(Boolean); }
	_set(a) { this._el.setAttribute('class', a.join(' ')); }
	contains(c) { return this._get().includes(c); }
	add(...cs) { const a = this._get(); for (const c of cs) if (!a.includes(c)) a.push(c); this._set(a); }
	remove(...cs) { this._set(this._get().filter(x => !cs.includes(x))); }
	toggle(c, force) { const has = this.contains(c); const want = force === undefined ? !has : !!force; if (want && !has) this.add(c); if (!want && has) this.remove(c); return want; }
	replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
	item(i) { return this._get()[i] || null; }
	get length() { return this._get().length; }
	get value() { return this._el.getAttribute('class') || ''; }
	toString() { return this.value; }
	forEach(f) { this._get().forEach(f); }
	[Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
function cssName(p) { return p.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); }
function styleObj(el) {
	const read = () => { const o = {}; for (const d of (el.getAttribute('style') || '').split(';')) { const i = d.indexOf(':'); if (i > 0) o[d.slice(0, i).trim().toLowerCase()] = d.slice(i + 1).trim(); } return o; };
	const write = o => { el.setAttribute('style', Object.keys(o).map(k => k + ':' + o[k]).join(';')); };
	const api = {
		setProperty(k, v) { const o = read(); if (v == null || v === '') delete o[k]; else o[k] = String(v); write(o); },
		getPropertyValue(k) { return read()[k] || ''; },
		removeProperty(k) { const o = read(); const v = o[k] || ''; delete o[k]; write(o); return v; },
	};
	return new Proxy(api, {
		get(t, p) { if (p in t) return t[p]; if (p === 'cssText') return el.getAttribute('style') || ''; if (typeof p !== 'string') return undefined; return read()[cssName(p)] || ''; },
		set(t, p, v) { if (p === 'cssText') { el.setAttribute('style', String(v)); return true; } api.setProperty(cssName(String(p)), v); return true; },
	});
}
function dataObj(el) {
	return new Proxy({}, {
		get(t, p) { return typeof p === 'string' ? el.getAttribute('data-' + cssName(p)) ?? undefined : undefined; },
		set(t, p, v) { el.setAttribute('data-' + cssName(String(p)), String(v)); return true; },
		deleteProperty(t, p) { el.removeAttribute('data-' + cssName(String(p))); return true; },
		has(t, p) { return el.hasAttribute('data-' + cssName(String(p))); },
		ownKeys() { return el.getAttributeNames().filter(n => n.startsWith('data-')).map(n => n.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase())); },
		getOwnPropertyDescriptor() { return { enumerable: true, configurable: true }; },
	});
}

/* --- HTML hacia fuera (innerHTML/outerHTML) */
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr']);
function esc(s, attr) { s = String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); return attr ? s.replace(/"/g, '&quot;') : s; }
function html(n) {
	const t = n.nodeType;
	if (t === TEXT) return esc(n.data);
	if (t === COMMENT) return '<!--' + n.data + '-->';
	if (t !== ELEMENT) return n.childNodes.map(html).join('');
	const tag = n.localName;
	let s = '<' + tag; for (const [k, v] of N.attrs(n._h)) s += ' ' + k + '="' + esc(v, true) + '"'; s += '>';
	if (VOID.has(tag)) return s;
	return s + n.childNodes.map(html).join('') + '</' + tag + '>';
}

class Element extends Node {
	get tagName() { return N.name(this._h).toUpperCase(); }
	get localName() { return N.name(this._h).toLowerCase(); }
	get id() { return this.getAttribute('id') || ''; }
	set id(v) { this.setAttribute('id', v); }
	get className() { return this.getAttribute('class') || ''; }
	set className(v) { this.setAttribute('class', v); }
	get classList() { return new DOMTokenList(this); }
	get style() { return styleObj(this); }
	set style(v) { this.setAttribute('style', String(v)); }
	get dataset() { return dataObj(this); }
	get attributes() { const a = N.attrs(this._h).map(([name, value]) => ({ name, value, nodeName: name, nodeValue: value })); a.getNamedItem = n => a.find(x => x.name === n) || null; return a; }
	getAttribute(n) { return N.getA(this._h, String(n).toLowerCase()); }
	setAttribute(n, v) {
		n = String(n).toLowerCase();
		const cb = this._ce && this.attributeChangedCallback, obs = cb && this.constructor.observedAttributes, old = obs && obs.includes(n) ? this.getAttribute(n) : undefined;
		N.setA(this._h, n, String(v));
		if ((n === 'width' || n === 'height') && this.localName === 'canvas') canvasResized(this);
		else if (n === 'src' && this.localName === 'img') imgLoaded(this);
		if (old !== undefined) try { this.attributeChangedCallback(n, old, String(v)); } catch (e) { report(e); }
	}
	removeAttribute(n) {
		n = String(n).toLowerCase();
		const cb = this._ce && this.attributeChangedCallback, obs = cb && this.constructor.observedAttributes, old = obs && obs.includes(n) ? this.getAttribute(n) : undefined;
		N.remA(this._h, n);
		if (old !== undefined && old !== null) try { this.attributeChangedCallback(n, old, null); } catch (e) { report(e); }
	}
	hasAttribute(n) { return N.getA(this._h, String(n).toLowerCase()) !== null; }
	toggleAttribute(n, f) { const h = this.hasAttribute(n); const want = f === undefined ? !h : !!f; if (want) this.setAttribute(n, ''); else this.removeAttribute(n); return want; }
	getAttributeNames() { return N.attrs(this._h).map(x => x[0]); }
	get children() { const a = new HTMLCollection(); for (let c = N.first(this._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) a.push(W(c)); return a; }
	get childElementCount() { return this.children.length; }
	get firstElementChild() { for (let c = N.first(this._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
	get lastElementChild() { for (let c = N.last(this._h); c; c = N.prev(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
	get nextElementSibling() { return nextEl(this); }
	get previousElementSibling() { return prevEl(this); }
	get innerHTML() { return this.childNodes.map(html).join(''); }
	set innerHTML(v) { while (this.firstChild) this.removeChild(this.firstChild); for (const h of N.parse(String(v))) N.append(this._h, h); if (ceDefs.size && this.isConnected) ceSubtree(this); }
	get outerHTML() { return html(this); }
	set outerHTML(v) { const p = this.parentNode; if (!p) return; for (const h of N.parse(String(v))) N.insert(p._h, h, this._h); this.remove(); }
	get innerText() { return this.textContent; }
	set innerText(v) { this.textContent = v; }
	insertAdjacentHTML(pos, v) {
		const nodes = N.parse(String(v)); pos = String(pos).toLowerCase();
		for (const h of nodes) {
			if (pos === 'beforebegin') this.parentNode && N.insert(this.parentNode._h, h, this._h);
			else if (pos === 'afterbegin') N.insert(this._h, h, N.first(this._h));
			else if (pos === 'beforeend') N.append(this._h, h);
			else if (pos === 'afterend') this.parentNode && N.insert(this.parentNode._h, h, N.next(this._h));
		}
	}
	insertAdjacentElement(pos, el) { pos = String(pos).toLowerCase(); if (pos === 'beforebegin') this.before(el); else if (pos === 'afterbegin') this.prepend(el); else if (pos === 'beforeend') this.append(el); else this.after(el); return el; }
	insertAdjacentText(pos, t) { this.insertAdjacentElement(pos, document.createTextNode(t)); }
	append(...ns) { for (const n of ns) this.appendChild(nodeOf(n)); }
	prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(nodeOf(n), f); }
	before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(nodeOf(n), this); }
	after(...ns) { const p = this.parentNode, nx = this.nextSibling; if (p) for (const n of ns) p.insertBefore(nodeOf(n), nx); }
	replaceWith(...ns) { this.before(...ns); this.remove(); }
	replaceChildren(...ns) { while (this.firstChild) this.removeChild(this.firstChild); this.append(...ns); }
	querySelector(s) { return qs(this, s); }
	querySelectorAll(s) { return qsa(this, s); }
	getElementsByTagName(t) { t = String(t).toLowerCase(); return qsa(this, t === '*' ? '*' : t); }
	getElementsByClassName(c) { return qsa(this, String(c).trim().split(/\s+/).map(x => '.' + x).join('')); }
	matches(s) { return matches(this, s); }
	closest(s) { for (let e = this; e && e.nodeType === ELEMENT; e = e.parentElement) if (matches(e, s)) return e; return null; }
	click() { this.dispatchEvent(new Event('click', { bubbles: true, cancelable: true })); }
	focus() {} blur() {} scrollIntoView() {} select() {}
	// medidas: las de la última vez que se colocó la página (en px de página, con la vista arriba del todo); lo que no
	// se colocó (display: none, fuera del documento) mide 0, como en un navegador
	getBoundingClientRect() { const r = N.rect(this._h); return r ? new DOMRect(r[0] - (G.scrollX || 0), r[1] - (G.scrollY || 0), r[2], r[3]) : new DOMRect(0, 0, 0, 0); }
	getClientRects() { return N.rect(this._h) ? [this.getBoundingClientRect()] : []; }
	get offsetWidth() { const r = N.rect(this._h); return r ? Math.round(r[2]) : 0; }
	get offsetHeight() { const r = N.rect(this._h); return r ? Math.round(r[3]) : 0; }
	get offsetTop() { const r = N.rect(this._h); return r ? Math.round(r[1]) : 0; }
	get offsetLeft() { const r = N.rect(this._h); return r ? Math.round(r[0]) : 0; }
	get offsetParent() { if (!N.rect(this._h)) return null; const b = document.body; return this === b || this === document.documentElement ? null : b; }
	get clientWidth() { if (this === document.documentElement) return G.innerWidth; const r = N.rect(this._h); return r ? Math.round(r[2]) : 0; }
	get clientHeight() { if (this === document.documentElement) return G.innerHeight; const r = N.rect(this._h); return r ? Math.round(r[3]) : 0; }
	get scrollWidth() { const r = this === document.documentElement || this === document.body ? N.rect(null) : N.rect(this._h); return r ? Math.round(r[2]) : 0; }
	get scrollHeight() { if (this === document.documentElement || this === document.body) { const r = N.rect(null); return Math.max(G.innerHeight, r ? Math.round(r[3]) : 0); } const r = N.rect(this._h); return r ? Math.round(r[3]) : 0; }
	get scrollTop() { return this === document.documentElement || this === document.body ? (G.scrollY || 0) : 0; } set scrollTop(v) {}
	get scrollLeft() { return 0; } set scrollLeft(v) {}
	attachShadow(o) { this._shadow = true; return this; }
	animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {} }; }
	// propiedades de formulario y enlaces que muchas páginas leen o cambian
	get value() { return this.getAttribute('value') ?? (this.localName === 'textarea' ? this.textContent : ''); }
	set value(v) { this.setAttribute('value', v); }
	get checked() { return this.hasAttribute('checked'); } set checked(v) { this.toggleAttribute('checked', !!v); }
	get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
	get hidden() { return this.hasAttribute('hidden'); } set hidden(v) { this.toggleAttribute('hidden', !!v); }
	get href() { const h = this.getAttribute('href'); return h == null ? '' : new URL(h, location.href).href; } set href(v) { this.setAttribute('href', v); }
	get src() { const h = this.getAttribute('src'); return h == null ? '' : new URL(h, location.href).href; } set src(v) { this.setAttribute('src', v); }
	get name() { return this.getAttribute('name') || ''; } set name(v) { this.setAttribute('name', v); }
	get type() { return this.getAttribute('type') || ''; } set type(v) { this.setAttribute('type', v); }
	get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); }
	get alt() { return this.getAttribute('alt') || ''; } set alt(v) { this.setAttribute('alt', v); }
	get rel() { return this.getAttribute('rel') || ''; } set rel(v) { this.setAttribute('rel', v); }
	get content() { return this.localName === 'template' ? (() => { const f = document.createDocumentFragment(); for (const c of this.childNodes) f.appendChild(c.cloneNode(true)); return f; })() : this.getAttribute('content'); }
	get text() { return this.textContent; } set text(v) { this.textContent = v; }
	get form() { return this.closest('form'); }
	get options() { return qsa(this, 'option'); }
	get selectedIndex() { return Math.max(0, this.options.findIndex(o => o.hasAttribute('selected'))); }
	get naturalWidth() { return imgNatural(this)[0]; } get naturalHeight() { return imgNatural(this)[1]; }
	get complete() { return true; }
	get dir() { return this.getAttribute('dir') || ''; } get lang() { return this.getAttribute('lang') || ''; }
	submit() { navigateForm(this); } reset() {}
	play() { return Promise.resolve(); } pause() {} load() {}
	getContext(t) { return this.localName === 'canvas' ? canvasCtx(this, t) : null; }
	toDataURL() { return this.localName === 'canvas' ? N.cvPng(N.cvNew(this._h, this.width, this.height)) : ''; }
	toBlob(cb, type) { const u = this.toDataURL(); setTimeout(() => cb(new G.Blob([G.atob(u.slice(u.indexOf(',') + 1))], { type: 'image/png' })), 0); }
	transferControlToOffscreen() { return this; }
	// más de formularios
	get selected() { return this.hasAttribute('selected'); } set selected(v) { this.toggleAttribute('selected', !!v); }
	get defaultValue() { return this.localName === 'textarea' ? this.textContent : this.getAttribute('value') || ''; } set defaultValue(v) { this.setAttribute('value', v); }
	get defaultChecked() { return this.hasAttribute('checked'); } set defaultChecked(v) { this.toggleAttribute('checked', !!v); }
	get defaultSelected() { return this.hasAttribute('selected'); }
	get required() { return this.hasAttribute('required'); } set required(v) { this.toggleAttribute('required', !!v); }
	get readOnly() { return this.hasAttribute('readonly'); } set readOnly(v) { this.toggleAttribute('readonly', !!v); }
	get multiple() { return this.hasAttribute('multiple'); } set multiple(v) { this.toggleAttribute('multiple', !!v); }
	get placeholder() { return this.getAttribute('placeholder') || ''; } set placeholder(v) { this.setAttribute('placeholder', v); }
	get maxLength() { const v = this.getAttribute('maxlength'); return v == null ? -1 : +v; } set maxLength(v) { this.setAttribute('maxlength', v); }
	get labels() { const id = this.id; return id ? qsa(document, 'label[for="' + id + '"]') : new NodeList(); }
	get validity() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; }
	get validationMessage() { return ''; } get willValidate() { return true; }
	checkValidity() { return true; } reportValidity() { return true; } setCustomValidity() {} requestSubmit() { this.submit(); }
	get elements() { return qsa(this, 'input,select,textarea,button,fieldset,output,object'); }
	setSelectionRange() {} setRangeText() {} get selectionStart() { return 0; } get selectionEnd() { return 0; }
	// enlaces: las partes de su dirección (<a>, <area>)
	get protocol() { return this._u().protocol; } get host() { return this._u().host; } get hostname() { return this._u().hostname; }
	get port() { return this._u().port; } get pathname() { return this._u().pathname; } get search() { return this._u().search; }
	get hash() { return this._u().hash; } get origin() { return this._u().origin; }
	_u() { try { return new URL(this.getAttribute('href') || '', location.href); } catch (e) { return new URL(location.href); } }
	get relList() { const el = this; return Object.assign(new DOMTokenList(el), { _get() { return (el.getAttribute('rel') || '').split(/\s+/).filter(Boolean); }, _set(a) { el.setAttribute('rel', a.join(' ')); }, supports: t => ['preload', 'prefetch', 'modulepreload', 'stylesheet', 'noopener', 'noreferrer'].includes(t) }); }
	get target() { return this.getAttribute('target') || ''; } set target(v) { this.setAttribute('target', v); }
	get download() { return this.getAttribute('download') || ''; }
	get nonce() { return this.getAttribute('nonce') || ''; } set nonce(v) { this.setAttribute('nonce', v); }
	get async() { return this.hasAttribute('async'); } set async(v) { this.toggleAttribute('async', !!v); }
	get defer() { return this.hasAttribute('defer'); } set defer(v) { this.toggleAttribute('defer', !!v); }
	get crossOrigin() { return this.getAttribute('crossorigin'); } set crossOrigin(v) { this.setAttribute('crossorigin', v); }
	get integrity() { return this.getAttribute('integrity') || ''; } set integrity(v) { this.setAttribute('integrity', v); }
	get loading() { return this.getAttribute('loading') || 'auto'; } set loading(v) { this.setAttribute('loading', v); }
	get srcset() { return this.getAttribute('srcset') || ''; } set srcset(v) { this.setAttribute('srcset', v); }
	get currentSrc() { return this.src; }
	decode() { return Promise.resolve(); }
	get width() { const a = this.getAttribute('width'); return this.localName === 'canvas' ? (a === null || !(+a >= 0) || a === '' ? 300 : Math.floor(+a)) : +a || 0; }
	set width(v) { this.setAttribute('width', this.localName === 'canvas' ? Math.max(0, Math.floor(+v) || 0) : v); }
	get height() { const a = this.getAttribute('height'); return this.localName === 'canvas' ? (a === null || !(+a >= 0) || a === '' ? 150 : Math.floor(+a)) : +a || 0; }
	set height(v) { this.setAttribute('height', this.localName === 'canvas' ? Math.max(0, Math.floor(+v) || 0) : v); }
	// varios
	get namespaceURI() { return this.localName === 'svg' || this.closest && this.closest('svg') ? 'http://www.w3.org/2000/svg' : 'http://www.w3.org/1999/xhtml'; }
	get prefix() { return null; }
	get tabIndex() { const v = this.getAttribute('tabindex'); return v == null ? (/^(a|button|input|select|textarea)$/.test(this.localName) ? 0 : -1) : +v; } set tabIndex(v) { this.setAttribute('tabindex', v); }
	get isContentEditable() { return this.getAttribute('contenteditable') === 'true' || this.getAttribute('contenteditable') === ''; }
	get contentEditable() { return this.getAttribute('contenteditable') || 'inherit'; } set contentEditable(v) { this.setAttribute('contenteditable', v); }
	get draggable() { return this.getAttribute('draggable') === 'true'; } get spellcheck() { return true; } get translate() { return true; }
	get slot() { return this.getAttribute('slot') || ''; } get assignedSlot() { return null; } get shadowRoot() { return this._shadow ? this : null; }
	get role() { return this.getAttribute('role'); } set role(v) { this.setAttribute('role', v); }
	get ariaLabel() { return this.getAttribute('aria-label'); } set ariaLabel(v) { this.setAttribute('aria-label', v); }
	get ariaHidden() { return this.getAttribute('aria-hidden'); } set ariaHidden(v) { this.setAttribute('aria-hidden', v); }
	get ariaExpanded() { return this.getAttribute('aria-expanded'); } set ariaExpanded(v) { this.setAttribute('aria-expanded', v); }
	get contentWindow() { if (this.localName !== 'iframe') return undefined; const id = N.frameOf(this._h); return id > 0 ? winProxy(id) : null; }
	get contentDocument() { return null; } // (otra página: su documento no se ve desde aquí, como entre orígenes distintos)
	webkitMatchesSelector(s) { return this.matches(s); } msMatchesSelector(s) { return this.matches(s); } mozMatchesSelector(s) { return this.matches(s); }
	getElementsByTagNameNS(ns, t) { return this.getElementsByTagName(t); }
	getAttributeNS(ns, n) { return this.getAttribute(n); } setAttributeNS(ns, n, v) { this.setAttribute(String(n).replace(/^\w+:/, ''), v); }
	removeAttributeNS(ns, n) { this.removeAttribute(n); } hasAttributeNS(ns, n) { return this.hasAttribute(n); }
	hasAttributes() { return N.attrs(this._h).length > 0; }
	getAttributeNode(n) { const v = this.getAttribute(n); return v == null ? null : { name: n, value: v, nodeName: n, nodeValue: v, specified: true }; }
	scroll() {} scrollTo() {} scrollBy() {}
	requestFullscreen() { return Promise.reject(new Error('no')); } requestPointerLock() {}
	setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
	getAnimations() { return []; }
	get clientTop() { return 0; } get clientLeft() { return 0; }
}
class Document extends Node {
	get documentElement() { for (let c = N.first(this._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
	get head() { return qs(this, 'head'); }
	get body() { return qs(this, 'body'); }
	get title() { const t = qs(this, 'title'); return t ? t.textContent : ''; }
	set title(v) { const t = qs(this, 'title'); if (t) t.textContent = v; }
	get URL() { return location.href; } get documentURI() { return location.href; }
	get readyState() { return G.__ready ? 'complete' : 'loading'; }
	get currentScript() { return G.__script || null; }
	get defaultView() { return G; }
	get cookie() { return N.cookie(); } // las mismas que van en las peticiones
	set cookie(v) { N.setCookie(String(v)); }
	get referrer() { return ''; } get domain() { return location.hostname; } get visibilityState() { return 'visible'; } get hidden() { return false; }
	get characterSet() { return 'UTF-8'; } get compatMode() { return 'CSS1Compat'; } get activeElement() { return this.body; }
	get forms() { return qsa(this, 'form'); } get images() { return qsa(this, 'img'); } get links() { return qsa(this, 'a[href]'); } get scripts() { return qsa(this, 'script'); }
	createElement(t, o) { const e = W(N.create(String(t).toLowerCase())); if (ceDefs.has(e.localName)) ceUpgrade(e); return e; }
	createElementNS(ns, t) { return this.createElement(t); }
	createTextNode(s) { return W(N.createText(String(s))); }
	createComment(s) { return W(N.createText('')); }
	createDocumentFragment() { return W(N.createFrag()); }
	createEvent() { return new Event(''); }
	createTreeWalker(root) { const a = []; walkEls(root, e => { a.push(e); }); let i = -1; return { nextNode() { i++; return a[i] || null; }, get currentNode() { return a[i] || root; } }; }
	getElementById(id) { return W(N.find(this._h, '', String(id), [], true)); }
	getElementsByTagName(t) { t = String(t).toLowerCase(); return qsa(this, t === '*' ? '*' : t); }
	getElementsByClassName(c) { return qsa(this, String(c).trim().split(/\s+/).map(x => '.' + x).join('')); }
	getElementsByName(n) { return qsa(this, '[name="' + n + '"]'); }
	querySelector(s) { return qs(this, s); }
	querySelectorAll(s) { return qsa(this, s); }
	write(...s) { const cur = G.__script; const nodes = N.parse(s.join('')); if (cur && cur.parentNode) { for (const h of nodes) N.insert(cur.parentNode._h, h, N.next(cur._h)); } else { const b = this.body; if (b) for (const h of nodes) N.append(b._h, h); } }
	writeln(...s) { this.write(...s, '\n'); }
	open() {} close() {}
	get implementation() {
		return {
			hasFeature: () => true,
			// un documento aparte (jQuery lo usa para leer HTML): aquí, un <html> suelto con su head y su body
			createHTMLDocument(title) {
				const html = document.createElement('html'), head = document.createElement('head'), body = document.createElement('body');
				html.appendChild(head); html.appendChild(body);
				const d = Object.create(Document.prototype);
				d._h = html._h; // las búsquedas van por su árbol
				Object.defineProperties(d, { documentElement: { value: html }, head: { value: head }, body: { value: body }, nodeType: { value: DOC } });
				d.createElement = t => document.createElement(t);
				d.createTextNode = t => document.createTextNode(t);
				d.createDocumentFragment = () => document.createDocumentFragment();
				d.createComment = t => document.createComment(t);
				return d;
			},
			createDocument() { return this.createHTMLDocument(''); },
		};
	}
	hasFocus() { return true; }
	execCommand() { return false; }
	get contentType() { return 'text/html'; } get inputEncoding() { return 'UTF-8'; } get charset() { return 'UTF-8'; }
	get children() { const a = new HTMLCollection(); const d = this.documentElement; if (d) a.push(d); return a; }
	get firstElementChild() { return this.documentElement; } get lastElementChild() { return this.documentElement; } get childElementCount() { return 1; }
	get scrollingElement() { return this.documentElement; } get fullscreenElement() { return null; } get pointerLockElement() { return null; }
	get styleSheets() { return []; } get fonts() { return { ready: Promise.resolve(), check: () => true, load: () => Promise.resolve([]), add() {}, addEventListener() {}, forEach() {}, status: 'loaded' }; }
	get dir() { return this.documentElement ? this.documentElement.getAttribute('dir') || '' : ''; }
	get prerendering() { return false; } get wasDiscarded() { return false; } get designMode() { return 'off'; }
	get lastModified() { return new Date().toLocaleString(); }
	getSelection() { return selection; }
	createRange() { return new Range(); }
	createNodeIterator(root, what, filter) { const a = []; walkAll(root, n => { a.push(n); }); let i = -1; return { nextNode() { i++; return a[i] || null; }, previousNode() { i--; return a[i] || null; }, detach() {} }; }
	createAttribute(n) { return { name: n, value: '', nodeName: n, nodeValue: '' }; }
	importNode(n, deep) { return n.cloneNode(deep); }
	adoptNode(n) { return n; }
	elementFromPoint() { return this.body; } elementsFromPoint() { return [this.body]; }
	exitFullscreen() { return Promise.resolve(); }
	append(...ns) { for (const n of ns) this.appendChild(nodeOf(n)); } prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(nodeOf(n), f); }
	replaceChildren(...ns) { while (this.firstChild) this.removeChild(this.firstChild); this.append(...ns); }
	startViewTransition(f) { try { f && f(); } catch (e) { report(e); } return { finished: Promise.resolve(), ready: Promise.resolve(), updateCallbackDone: Promise.resolve(), skipTransition() {} }; }
}
class DocumentFragment extends Node {
	querySelector(s) { return qs(this, s); }
	querySelectorAll(s) { return qsa(this, s); }
	getElementById(id) { return qs(this, '#' + id); }
	get children() { return this.childNodes.filter(n => n.nodeType === ELEMENT); }
	append(...ns) { for (const n of ns) this.appendChild(nodeOf(n)); }
}

/* ---------- window */
const document = W(N.doc());
G.window = G; G.self = G; G.globalThis = G; G.top = G; G.parent = G; G.frames = G;
G.document = document;
G.Node = Node; G.Element = Element; G.HTMLElement = Element; G.Text = Text; G.Comment = Comment; G.Document = Document; G.HTMLDocument = Document;
G.DocumentFragment = DocumentFragment; G.CharacterData = CharacterData; G.EventTarget = EventTarget; G.Event = Event; G.CustomEvent = CustomEvent;
for (const k of ['MouseEvent', 'KeyboardEvent', 'FocusEvent', 'UIEvent', 'InputEvent', 'TouchEvent', 'PointerEvent', 'ErrorEvent', 'MessageEvent', 'ProgressEvent', 'PopStateEvent', 'HashChangeEvent', 'StorageEvent', 'AnimationEvent', 'TransitionEvent']) G[k] = class extends Event {};
for (const k of ['HTMLDivElement', 'HTMLSpanElement', 'HTMLAnchorElement', 'HTMLImageElement', 'HTMLInputElement', 'HTMLButtonElement', 'HTMLFormElement', 'HTMLScriptElement', 'HTMLLinkElement', 'HTMLStyleElement', 'HTMLIFrameElement', 'HTMLVideoElement', 'HTMLAudioElement', 'HTMLMediaElement', 'HTMLCanvasElement', 'HTMLTemplateElement', 'HTMLSelectElement', 'HTMLTextAreaElement', 'HTMLBodyElement', 'HTMLHeadElement', 'HTMLHtmlElement', 'HTMLParagraphElement', 'HTMLUListElement', 'HTMLLIElement', 'HTMLTableElement', 'SVGElement', 'HTMLUnknownElement'])
	G[k] = Element;

/* ---------- web components: customElements.define, «class X extends HTMLElement», sus callbacks */
const ceDefs = new Map(), ceByClass = new Map(), ceWait = new Map();
let ceUpgrading = null;
// HTMLElement como constructor: «super()» en un componente devuelve el elemento que se está mejorando (o uno nuevo)
function HTMLElementCtor() {
	if (ceUpgrading) { const e = ceUpgrading; ceUpgrading = null; return e; }
	const name = new.target && ceByClass.get(new.target);
	const e = W(N.create(name || 'div'));
	if (new.target) Object.setPrototypeOf(e, new.target.prototype);
	e._ce = true;
	return e;
}
HTMLElementCtor.prototype = Element.prototype; // (instanceof HTMLElement: todos los elementos)
Object.setPrototypeOf(HTMLElementCtor, Element);
G.HTMLElement = HTMLElementCtor;
function ceUpgrade(el) {
	if (el._ce || el.nodeType !== ELEMENT) return;
	const cls = ceDefs.get(el.localName);
	if (!cls) return;
	el._ce = true;
	Object.setPrototypeOf(el, cls.prototype);
	ceUpgrading = el;
	try { new cls(); } catch (e) { report(e); }
	ceUpgrading = null;
	const obs = cls.observedAttributes;
	if (obs && typeof el.attributeChangedCallback === 'function')
		for (const a of obs) { const v = el.getAttribute(a); if (v !== null) try { el.attributeChangedCallback(a, null, v); } catch (e) { report(e); } }
	if (el.isConnected && typeof el.connectedCallback === 'function') { el._conn = true; try { el.connectedCallback(); } catch (e) { report(e); } }
}
// lo que entra en el documento: sus componentes, mejorados (y avisados de que ya están)
function ceSubtree(root) {
	if (!ceDefs.size || !root || !root._h) return;
	const one = el => { if (ceDefs.has(el.localName)) { if (!el._ce) ceUpgrade(el); else if (el.isConnected && typeof el.connectedCallback === 'function' && !el._conn) { el._conn = true; try { el.connectedCallback(); } catch (e) { report(e); } } } };
	if (root.nodeType === ELEMENT) one(root);
	if (root.nodeType === ELEMENT || root.nodeType === FRAG || root.nodeType === DOC) for (const h of N.find(root._h, '*-', '', [], false) || []) one(W(h)); // (solo las de guion)
}
G.customElements = {
	define(name, cls) {
		name = String(name).toLowerCase();
		if (ceDefs.has(name)) throw new DOMException('"' + name + '" ya está definido', 'NotSupportedError');
		ceDefs.set(name, cls); ceByClass.set(cls, name);
		for (const h of N.find(document._h, name, '', [], false) || []) ceUpgrade(W(h));
		const w = ceWait.get(name); if (w) { w.res(cls); ceWait.delete(name); }
	},
	get(name) { return ceDefs.get(String(name).toLowerCase()); },
	getName(cls) { return ceByClass.get(cls) || null; },
	whenDefined(name) { name = String(name).toLowerCase(); if (ceDefs.has(name)) return Promise.resolve(ceDefs.get(name)); let w = ceWait.get(name); if (!w) { let res; const p = new Promise(r => { res = r; }); w = { p, res }; ceWait.set(name, w); } return w.p; },
	upgrade(root) { ceSubtree(root); },
};
// shadow DOM, lo básico: su raíz es el propio elemento (lo que pone en ella se ve como hijos suyos)
G.ShadowRoot = class {};
const proto = EventTarget.prototype;
G.addEventListener = proto.addEventListener.bind(G); G.removeEventListener = proto.removeEventListener.bind(G); G.dispatchEvent = proto.dispatchEvent.bind(G);
G.__cookies = []; G.__ready = false; G.__script = null;

const loc0 = N.url;
class URLx {
	constructor(u, base) {
		u = String(u);
		const m0 = /^([a-zA-Z][\w+.-]*:)/.exec(u);
		if (!m0 && base !== undefined) u = resolveUrl(String(base), u);
		const m = /^([a-zA-Z][\w+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(u);
		if (!m) throw new TypeError('URL no válida: ' + u);
		this.protocol = m[1].toLowerCase(); this.host = m[2] || ''; this._path = m[3] || '/'; this._search = m[4] || ''; this._hash = m[5] || '';
		this.username = ''; this.password = '';
	}
	// (como en un navegador: search y hash con su «?» y «#», o vacíos; al ponerlos, se añaden si faltan)
	get search() { return this._search.length > 1 ? this._search : ''; }
	set search(v) { v = String(v); this._search = v === '' ? '' : v[0] === '?' ? v : '?' + v; }
	get hash() { return this._hash.length > 1 ? this._hash : ''; }
	set hash(v) { v = String(v); this._hash = v === '' ? '' : v[0] === '#' ? v : '#' + v; }
	get pathname() { return this._path; }
	set pathname(v) { v = String(v); this._path = v[0] === '/' ? v : '/' + v; }
	get hostname() { return this.host.split(':')[0]; }
	set hostname(v) { const p = this.port; this.host = String(v) + (p ? ':' + p : ''); }
	get port() { return this.host.split(':')[1] || ''; }
	set port(v) { v = String(v); this.host = this.hostname + (v ? ':' + v : ''); }
	get origin() { return this.protocol + '//' + this.host; }
	get href() { return this.protocol + (this.host || this.protocol === 'file:' ? '//' + this.host : '') + this._path + this._search + this._hash; }
	set href(v) { const n = new URLx(v); this.protocol = n.protocol; this.host = n.host; this._path = n._path; this._search = n._search; this._hash = n._hash; }
	get searchParams() { const self = this; const p = new URLSearchParams(this.search); p._url = self; return p; }
	toString() { return this.href; }
	toJSON() { return this.href; }
}
function resolveUrl(base, rel) {
	if (/^[a-zA-Z][\w+.-]*:/.test(rel)) return rel;
	const b = /^([a-zA-Z][\w+.-]*:)(\/\/[^/?#]*)?([^?#]*)(\?[^#]*)?/.exec(base) || ['', 'https:', '', '/', ''];
	if (rel.startsWith('//')) return b[1] + rel;
	if (rel.startsWith('/')) return b[1] + (b[2] || '') + rel;
	if (rel.startsWith('?')) return b[1] + (b[2] || '') + b[3] + rel;
	if (rel.startsWith('#')) return base.replace(/#.*$/, '') + rel;
	const dir = b[3].replace(/[^/]*$/, '');
	const parts = (dir + rel).split('/'); const out = [];
	for (const p of parts) { if (p === '..') out.pop(); else if (p !== '.') out.push(p); }
	return b[1] + (b[2] || '') + out.join('/');
}
class URLSearchParams {
	constructor(q) {
		this._p = [];
		if (q && typeof q === 'object') { for (const k of Object.keys(q)) this._p.push([k, String(q[k])]); return; }
		q = String(q || '').replace(/^\?/, '');
		for (const kv of q.split('&')) if (kv) { const i = kv.indexOf('='); const dec = s => decodeURIComponent(s.replace(/\+/g, ' ')); this._p.push(i < 0 ? [dec(kv), ''] : [dec(kv.slice(0, i)), dec(kv.slice(i + 1))]); }
	}
	get(k) { const e = this._p.find(x => x[0] === k); return e ? e[1] : null; }
	getAll(k) { return this._p.filter(x => x[0] === k).map(x => x[1]); }
	has(k) { return this._p.some(x => x[0] === k); }
	set(k, v) { this.delete(k); this._p.push([k, String(v)]); this._sync(); }
	append(k, v) { this._p.push([k, String(v)]); this._sync(); }
	delete(k) { this._p = this._p.filter(x => x[0] !== k); this._sync(); }
	forEach(f) { for (const [k, v] of this._p) f(v, k, this); }
	entries() { return this._p[Symbol.iterator](); } keys() { return this._p.map(x => x[0])[Symbol.iterator](); } values() { return this._p.map(x => x[1])[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
	toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); }
	_sync() { if (this._url) { const s = this.toString(); this._url.search = s ? '?' + s : ''; } }
}
G.URL = URLx; G.URLSearchParams = URLSearchParams;
URLx.createObjectURL = () => 'blob:'; URLx.revokeObjectURL = () => {};

const locObj = new URLx(/^[a-zA-Z][\w+.-]*:/.test(loc0) ? loc0 : 'file://' + loc0);
G.location = new Proxy(locObj, {
	set(t, p, v) {
		if (p === 'href') N.navigate(resolveUrl(t.href, String(v)));
		else if (p === 'hash') t.hash = String(v).startsWith('#') ? v : '#' + v;
		else if (p === 'search') N.navigate(t.protocol + '//' + t.host + t.pathname + (String(v).startsWith('?') ? v : '?' + v));
		else t[p] = v;
		return true;
	},
	get(t, p) {
		if (p === 'assign' || p === 'replace') return u => N.navigate(resolveUrl(t.href, String(u)));
		if (p === 'reload') return () => N.navigate(t.href); // (otra vez la misma: tras un reto «soy un navegador», ya con su cookie)
		if (p === 'toString') return () => t.href;
		return t[p];
	},
});
document.location = G.location;

G.navigator = { userAgent: N.ua, language: 'es-ES', languages: ['es-ES', 'es', 'en'], platform: 'Nintendo 3DS', cookieEnabled: true, onLine: true,
	vendor: '', appName: 'Netscape', appVersion: '5.0', maxTouchPoints: 1, hardwareConcurrency: 1, webdriver: false,
	sendBeacon() { return true; }, serviceWorker: undefined, clipboard: { writeText: () => Promise.resolve() }, mediaDevices: undefined };
G.screen = { width: N.width, height: 600, availWidth: N.width, availHeight: 600, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };
G.innerWidth = N.width; G.innerHeight = N.height || 600; G.outerWidth = N.width; G.outerHeight = N.height || 600; G.devicePixelRatio = 1;
G.scrollX = 0; G.scrollY = 0; G.pageXOffset = 0; G.pageYOffset = 0;
G.scrollTo = G.scroll = G.scrollBy = () => {};
G.alert = m => N.log('alert: ' + m); G.confirm = () => true; G.prompt = () => null; G.print = () => {};
G.open = u => { if (u) N.navigate(resolveUrl(location.href, String(u))); return null; };
G.close = () => {}; G.focus = () => {}; G.blur = () => {}; G.stop = () => {};
G.getComputedStyle = el => { const s = el && el.style; return new Proxy({}, { get(t, p) { if (p === 'getPropertyValue') return k => (s ? s.getPropertyValue(k) : ''); if (p === 'display') return el && el.hidden ? 'none' : ((s && s.display) || 'block'); return (s && typeof p === 'string' && s[p]) || ''; } }); };
G.matchMedia = q => ({ matches: /max-width/.test(q) ? N.width <= (+(/(\d+)/.exec(q) || [0, 0])[1]) : /prefers-color-scheme:\s*dark/.test(q) ? !!N.dark : /prefers-color-scheme:\s*light/.test(q) ? !N.dark : /min-width/.test(q) && N.width >= (+(/(\d+)/.exec(q) || [0, 0])[1]),
	media: q, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} });
G.history = { length: 1, state: null, pushState(s, t, u) { this.state = s; }, replaceState(s, t, u) { this.state = s; }, back() {}, forward() {}, go() {} };
const store = () => { const m = new Map(); return { getItem: k => (m.has(String(k)) ? m.get(String(k)) : null), setItem: (k, v) => m.set(String(k), String(v)), removeItem: k => m.delete(String(k)), clear: () => m.clear(), key: i => [...m.keys()][i] ?? null, get length() { return m.size; } }; };
G.localStorage = store(); G.sessionStorage = store();
G.console = { log: (...a) => N.log('console: ' + a.map(String).join(' ')), info() {}, debug() {}, warn() {}, error: (...a) => N.log('console.error: ' + a.map(x => x && x.stack ? x + ' @ ' + x.stack.split('\n').slice(0, 4).join(' | ') : String(x)).join(' ')),
	trace() {}, group() {}, groupEnd() {}, groupCollapsed() {}, table() {}, time() {}, timeEnd() {}, assert() {}, dir() {}, count() {} };
G.performance = { now: () => N.now(), timing: { navigationStart: 0 }, mark() {}, measure() {}, getEntriesByType: () => [], getEntriesByName: () => [] };
G.crypto = { getRandomValues: a => { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256); return a; }, randomUUID: () => 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { const r = Math.random() * 16 | 0; return (c === 'x' ? r : r & 3 | 8).toString(16); }) };
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
G.btoa = s => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2); o += B64[a >> 2] + B64[(a & 3) << 4 | (b >> 4 || 0)] + (isNaN(b) ? '=' : B64[(b & 15) << 2 | (c >> 6 || 0)]) + (isNaN(c) ? '=' : B64[c & 63]); } return o; };
G.atob = s => { s = String(s).replace(/[^A-Za-z0-9+/]/g, ''); let o = ''; for (let i = 0; i < s.length; i += 4) { const a = B64.indexOf(s[i]), b = B64.indexOf(s[i + 1]), c = B64.indexOf(s[i + 2]), d = B64.indexOf(s[i + 3]); o += String.fromCharCode(a << 2 | b >> 4); if (c >= 0 && s[i + 2]) o += String.fromCharCode((b & 15) << 4 | c >> 2); if (d >= 0 && s[i + 3]) o += String.fromCharCode((c & 3) << 6 | d); } return o; };
class Observer { constructor(cb) { this.cb = cb; } observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } }
G.MutationObserver = Observer; G.ResizeObserver = Observer; G.PerformanceObserver = Observer;
// (todo «se ve»: la página se pinta entera, y así las imágenes perezosas se piden; con su rectángulo de verdad)
G.IntersectionObserver = class extends Observer { observe(el) { const cb = this.cb; setTimeout(() => { try { const r = el.getBoundingClientRect(); cb([{ target: el, isIntersecting: true, isVisible: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: new DOMRect(0, 0, G.innerWidth, G.innerHeight), time: N.now() }], this); } catch (e) { report(e); } }, 0); } };
G.requestIdleCallback = f => setTimeout(() => f({ timeRemaining: () => 50, didTimeout: false }), 1);
G.cancelIdleCallback = id => clearTimeout(id);
/* ---------- <canvas> 2D: CanvasRenderingContext2D sobre los natives cv* (canvas.c dibuja los píxeles) */
const CNAMES = 'aliceblue:f0f8ff,antiquewhite:faebd7,aqua:0ff,aquamarine:7fffd4,azure:f0ffff,beige:f5f5dc,bisque:ffe4c4,black:000,blanchedalmond:ffebcd,blue:00f,blueviolet:8a2be2,brown:a52a2a,burlywood:deb887,cadetblue:5f9ea0,chartreuse:7fff00,chocolate:d2691e,coral:ff7f50,cornflowerblue:6495ed,cornsilk:fff8dc,crimson:dc143c,cyan:0ff,darkblue:00008b,darkcyan:008b8b,darkgoldenrod:b8860b,darkgray:a9a9a9,darkgreen:006400,darkgrey:a9a9a9,darkkhaki:bdb76b,darkmagenta:8b008b,darkolivegreen:556b2f,darkorange:ff8c00,darkorchid:9932cc,darkred:8b0000,darksalmon:e9967a,darkseagreen:8fbc8f,darkslateblue:483d8b,darkslategray:2f4f4f,darkslategrey:2f4f4f,darkturquoise:00ced1,darkviolet:9400d3,deeppink:ff1493,deepskyblue:00bfff,dimgray:696969,dimgrey:696969,dodgerblue:1e90ff,firebrick:b22222,floralwhite:fffaf0,forestgreen:228b22,fuchsia:f0f,gainsboro:dcdcdc,ghostwhite:f8f8ff,gold:ffd700,goldenrod:daa520,gray:808080,green:008000,greenyellow:adff2f,grey:808080,honeydew:f0fff0,hotpink:ff69b4,indianred:cd5c5c,indigo:4b0082,ivory:fffff0,khaki:f0e68c,lavender:e6e6fa,lavenderblush:fff0f5,lawngreen:7cfc00,lemonchiffon:fffacd,lightblue:add8e6,lightcoral:f08080,lightcyan:e0ffff,lightgoldenrodyellow:fafad2,lightgray:d3d3d3,lightgreen:90ee90,lightgrey:d3d3d3,lightpink:ffb6c1,lightsalmon:ffa07a,lightseagreen:20b2aa,lightskyblue:87cefa,lightslategray:789,lightslategrey:789,lightsteelblue:b0c4de,lightyellow:ffffe0,lime:0f0,limegreen:32cd32,linen:faf0e6,magenta:f0f,maroon:800000,mediumaquamarine:66cdaa,mediumblue:0000cd,mediumorchid:ba55d3,mediumpurple:9370db,mediumseagreen:3cb371,mediumslateblue:7b68ee,mediumspringgreen:00fa9a,mediumturquoise:48d1cc,mediumvioletred:c71585,midnightblue:191970,mintcream:f5fffa,mistyrose:ffe4e1,moccasin:ffe4b5,navajowhite:ffdead,navy:000080,oldlace:fdf5e6,olive:808000,olivedrab:6b8e23,orange:ffa500,orangered:ff4500,orchid:da70d6,palegoldenrod:eee8aa,palegreen:98fb98,paleturquoise:afeeee,palevioletred:db7093,papayawhip:ffefd5,peachpuff:ffdab9,peru:cd853f,pink:ffc0cb,plum:dda0dd,powderblue:b0e0e6,purple:800080,rebeccapurple:639,red:f00,rosybrown:bc8f8f,royalblue:4169e1,saddlebrown:8b4513,salmon:fa8072,sandybrown:f4a460,seagreen:2e8b57,seashell:fff5ee,sienna:a0522d,silver:c0c0c0,skyblue:87ceeb,slateblue:6a5acd,slategray:708090,slategrey:708090,snow:fffafa,springgreen:00ff7f,steelblue:4682b4,tan:d2b48c,teal:008080,thistle:d8bfd8,tomato:ff6347,turquoise:40e0d0,violet:ee82ee,wheat:f5deb3,white:fff,whitesmoke:f5f5f5,yellow:ff0,yellowgreen:9acd32';
let cnames = null;
// un color de CSS -> [r, g, b, a] (a de 0 a 1); null si no se entiende
function cssColor(s) {
	s = String(s).trim().toLowerCase();
	if (s === 'transparent') return [0, 0, 0, 0];
	if (!cnames) { cnames = new Map(); for (const kv of CNAMES.split(',')) { const [k, v] = kv.split(':'); cnames.set(k, v); } }
	if (cnames.has(s)) s = '#' + cnames.get(s);
	let m;
	if (s[0] === '#') {
		let h = s.slice(1);
		if (h.length === 3 || h.length === 4) h = h.replace(/./g, c => c + c);
		if (!/^[0-9a-f]{6}([0-9a-f]{2})?$/.test(h)) return null;
		return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16), h.length === 8 ? parseInt(h.slice(6), 16) / 255 : 1];
	}
	if ((m = /^(rgba?|hsla?)\(([^)]*)\)$/.exec(s))) {
		const p = m[2].split(/[\s,\/]+/).filter(Boolean);
		if (p.length < 3) return null;
		const a = p.length > 3 ? (p[3].endsWith('%') ? parseFloat(p[3]) / 100 : parseFloat(p[3])) : 1;
		if (m[1][0] === 'r') {
			const c = p.slice(0, 3).map(v => v.endsWith('%') ? parseFloat(v) * 2.55 : parseFloat(v));
			return [...c.map(v => Math.max(0, Math.min(255, Math.round(v)))), Math.max(0, Math.min(1, a))];
		}
		const hh = ((parseFloat(p[0]) % 360) + 360) % 360 / 360, ss = parseFloat(p[1]) / 100, ll = parseFloat(p[2]) / 100;
		const q = ll < 0.5 ? ll * (1 + ss) : ll + ss - ll * ss, pp = 2 * ll - q;
		const f = t => { t = (t + 1) % 1; return t < 1 / 6 ? pp + (q - pp) * 6 * t : t < 0.5 ? q : t < 2 / 3 ? pp + (q - pp) * (2 / 3 - t) * 6 : pp; };
		return [Math.round(f(hh + 1 / 3) * 255), Math.round(f(hh) * 255), Math.round(f(hh - 1 / 3) * 255), Math.max(0, Math.min(1, a))];
	}
	return null;
}
const hex2 = v => (v | 0).toString(16).padStart(2, '0');
// como lo devuelve un navegador: #rrggbb, o rgba(…) si es transparente
function colorStr(c) { return c[3] >= 1 ? '#' + hex2(c[0]) + hex2(c[1]) + hex2(c[2]) : 'rgba(' + c[0] + ', ' + c[1] + ', ' + c[2] + ', ' + c[3] + ')'; }
const packColor = c => ((Math.round(c[3] * 255) << 24) >>> 0) + (c[2] << 16) + (c[1] << 8) + c[0];
class CanvasGradient {
	constructor(type, g) { this._type = type; this._g = g.map(Number); this._stops = []; }
	addColorStop(o, c) {
		o = +o; if (!(o >= 0 && o <= 1)) throw new DOMException('offset', 'IndexSizeError');
		const v = cssColor(c); if (!v) throw new DOMException('color', 'SyntaxError');
		let i = this._stops.length; while (i > 0 && this._stops[i - 1][0] > o) i--; // (estable: los iguales, en orden)
		this._stops.splice(i, 0, [o, v]);
	}
	_flat() { const s = this._stops; return s.length ? s[s.length >> 1][1] : [0, 0, 0, 0]; }
	_send(id, stroke) { const st = []; for (const [o, c] of this._stops) st.push(o, packColor(c)); const g = this._g; N.cvPaint(id, stroke, this._type, g[0] || 0, g[1] || 0, g[2] || 0, g[3] || 0, g[4] || 0, g[5] || 0, st, 0, 0); }
}
class CanvasPattern {
	constructor(img, rep) { this._img = img; this._rep = rep; }
	setTransform() {}
	_flat() { return [128, 128, 128, 1]; }
	_send(id, stroke) { N.cvPaint(id, stroke, 4, 0, 0, 0, 0, 0, 0, [], this._img, this._rep); }
}
// de dónde sale una imagen para drawImage / createPattern: [número, ancho, alto, es un canvas]
function imgSource(img) {
	if (!img) return null;
	if (img._bitmap) return img._bitmap;
	if (img._h && img.localName === 'canvas') { const id = img._cvid || (img._cvid = N.cvNew(img._h, img.width, img.height)); return id ? [id, img.width, img.height, true] : null; }
	if (img._h && (img.localName === 'img' || img.localName === 'image')) {
		const src = img.currentSrc || img.src || (img.getAttribute('href') || '');
		if (!src) return null;
		const r = N.cvImg(new URL(src, location.href).href);
		if (r) img._nat = [r[1], r[2]];
		return r ? [r[0], r[1], r[2], false] : null;
	}
	return null;
}
// las <img>: al poner src, «load» (un momento después; los píxeles se bajan cuando se dibujan)
function imgLoaded(el) {
	el._nat = undefined;
	setTimeout(() => { try { el.dispatchEvent(new Event('load')); } catch (e) { report(e); } }, 0);
}
function imgNatural(el) {
	if (el._nat) return el._nat;
	// (las que no están en la página, como new Image(): se bajan para saber su tamaño; las de la página, el de sus atributos)
	if (el.localName === 'img' && !el.isConnected && el.getAttribute('src')) { const r = imgSource(el); if (r) return el._nat; }
	return [+el.getAttribute('width') || 0, +el.getAttribute('height') || 0];
}
class ImageData {
	constructor(a, b, c) {
		if (a instanceof Uint8ClampedArray) { this.data = a; this.width = b; this.height = c || a.length / 4 / b; }
		else { this.width = a | 0; this.height = b | 0; this.data = new Uint8ClampedArray(this.width * this.height * 4); }
		this.colorSpace = 'srgb';
	}
}
class Path2D {
	constructor(p) { this._ops = p instanceof Path2D ? p._ops.slice() : []; if (typeof p === 'string') svgPath(this, p); }
	addPath(p) { if (p && p._ops) this._ops.push(...p._ops); }
}
for (const k of ['moveTo', 'lineTo', 'bezierCurveTo', 'quadraticCurveTo', 'arc', 'arcTo', 'ellipse', 'rect', 'roundRect', 'closePath'])
	Path2D.prototype[k] = function (...a) { this._ops.push([k, a]); };
// los «d» de SVG en un Path2D (M L H V C S Q T A Z, también en minúsculas)
function svgPath(p, d) {
	const t = String(d).match(/[a-df-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:e[-+]?\d+)?/gi) || [];
	let i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0, lc = null;
	const n = () => +t[i++];
	while (i < t.length) {
		if (/[a-z]/i.test(t[i])) cmd = t[i++];
		const rel = cmd === cmd.toLowerCase(), C = cmd.toUpperCase(), ox = rel ? x : 0, oy = rel ? y : 0;
		if (C === 'Z') { p.closePath(); x = sx, y = sy; lc = null; continue; }
		if (C === 'M') { x = ox + n(), y = oy + n(); sx = x, sy = y; p.moveTo(x, y); cmd = rel ? 'l' : 'L'; lc = null; }
		else if (C === 'L') { x = ox + n(), y = oy + n(); p.lineTo(x, y); lc = null; }
		else if (C === 'H') { x = ox + n(); p.lineTo(x, y); lc = null; }
		else if (C === 'V') { y = oy + n(); p.lineTo(x, y); lc = null; }
		else if (C === 'C' || C === 'S') {
			let c1x, c1y;
			if (C === 'C') c1x = ox + n(), c1y = oy + n(); else [c1x, c1y] = lc && lc[2] === 'c' ? [2 * x - lc[0], 2 * y - lc[1]] : [x, y];
			const c2x = ox + n(), c2y = oy + n(); x = ox + n(), y = oy + n();
			p.bezierCurveTo(c1x, c1y, c2x, c2y, x, y); lc = [c2x, c2y, 'c'];
		} else if (C === 'Q' || C === 'T') {
			let qx, qy;
			if (C === 'Q') qx = ox + n(), qy = oy + n(); else [qx, qy] = lc && lc[2] === 'q' ? [2 * x - lc[0], 2 * y - lc[1]] : [x, y];
			x = ox + n(), y = oy + n(); p.quadraticCurveTo(qx, qy, x, y); lc = [qx, qy, 'q'];
		} else if (C === 'A') {
			let rx = Math.abs(n()), ry = Math.abs(n()); const rot = n() * Math.PI / 180, large = n(), sweep = n(), x2 = ox + n(), y2 = oy + n();
			// a centro (SVG 1.1, F.6.5)
			if (!rx || !ry) p.lineTo(x2, y2);
			else {
				const cs = Math.cos(rot), sn = Math.sin(rot), dx = (x - x2) / 2, dy = (y - y2) / 2, x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
				const l = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry); if (l > 1) rx *= Math.sqrt(l), ry *= Math.sqrt(l);
				let k = Math.sqrt(Math.max(0, (rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1) / (rx * rx * y1 * y1 + ry * ry * x1 * x1)));
				if (large === sweep) k = -k;
				const cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx, cx = cs * cx1 - sn * cy1 + (x + x2) / 2, cy = sn * cx1 + cs * cy1 + (y + y2) / 2;
				const a0 = Math.atan2((y1 - cy1) / ry, (x1 - cx1) / rx), a1 = Math.atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx);
				let da = a1 - a0; if (sweep && da < 0) da += 2 * Math.PI; else if (!sweep && da > 0) da -= 2 * Math.PI;
				p.ellipse(cx, cy, rx, ry, rot, a0, a0 + da, !sweep);
			}
			x = x2, y = y2; lc = null;
		} else i++;
	}
}
const CAPS = { butt: 0, round: 1, square: 2 }, JOINS = { miter: 0, round: 1, bevel: 2 };
const COMPS = { 'source-over': 0, 'destination-out': 1, copy: 2, 'destination-over': 3, 'source-atop': 4, lighter: 5, 'source-in': 6, 'destination-in': 7, xor: 8,
	'destination-atop': 9, multiply: 10, screen: 11, darken: 12, lighten: 13, overlay: 14, 'hard-light': 15, difference: 16, exclusion: 16, 'color-dodge': 13, 'color-burn': 12, 'soft-light': 14 };
// «bold 12px Arial» -> [px, negrita]
function fontOf(f) {
	const m = /(?:^|\s)(\d*\.?\d+)(px|pt|em|rem|%)?(?:\s*\/\s*\S+)?\s+/.exec(String(f) + ' ');
	let px = 10;
	if (m) { px = +m[1]; const u = m[2] || 'px'; px = u === 'pt' ? px * 4 / 3 : u === 'em' || u === 'rem' ? px * 16 : u === '%' ? px * 0.16 : px; }
	return [px, /\b(bold|bolder|[6-9]00)\b/.test(f)];
}
const ctxs = new Map(); // id del lienzo -> su contexto
class CanvasRenderingContext2D {
	constructor(canvas, id) {
		this.canvas = canvas; this._id = id; this._stack = [];
		this._reset();
	}
	_reset() {
		this._s = { fill: [0, 0, 0, 1], fillStyle: '#000000', stroke: [0, 0, 0, 1], strokeStyle: '#000000', globalAlpha: 1, lineWidth: 1, lineCap: 'butt', lineJoin: 'miter',
			miterLimit: 10, globalCompositeOperation: 'source-over', font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic', direction: 'inherit',
			shadowBlur: 0, shadowColor: 'rgba(0, 0, 0, 0)', shadowOffsetX: 0, shadowOffsetY: 0, imageSmoothingEnabled: true, imageSmoothingQuality: 'low',
			lineDash: [], lineDashOffset: 0, filter: 'none', letterSpacing: '0px', fontKerning: 'auto', m: [1, 0, 0, 1, 0, 0] };
		this._stack = []; this._dirty = true;
	}
	_sync() {
		if (!this._dirty && !this._s.fillStyle._send && !this._s.strokeStyle._send) return; // (los degradados, siempre: pueden tener paradas nuevas)
		const s = this._s;
		N.cvStyle(this._id, packColor(s.fill), packColor(s.stroke), s.globalAlpha, s.lineWidth, CAPS[s.lineCap] || 0, JOINS[s.lineJoin] || 0, s.miterLimit, COMPS[s.globalCompositeOperation] || 0);
		if (s.fillStyle._send) s.fillStyle._send(this._id, false);
		if (s.strokeStyle._send) s.strokeStyle._send(this._id, true);
		const sc = cssColor(s.shadowColor) || [0, 0, 0, 0], d = s.lineDash.length & 1 ? s.lineDash.concat(s.lineDash) : s.lineDash;
		N.cvStyle2(this._id, d, s.lineDashOffset, s.imageSmoothingEnabled, packColor(sc), s.shadowOffsetX, s.shadowOffsetY, s.shadowBlur);
		this._dirty = false;
	}
	_setM(m) { this._s.m = m; N.cvSetT(this._id, ...m); }
	save() { this._stack.push({ ...this._s, m: this._s.m.slice(), lineDash: this._s.lineDash.slice() }); N.cvSave(this._id); }
	restore() { const s = this._stack.pop(); if (s) { this._s = s; this._dirty = true; N.cvSetT(this._id, ...s.m); N.cvRestore(this._id); } }
	reset() { this._reset(); N.cvResize(this._id, this.canvas.width, this.canvas.height); }
	isContextLost() { return false; }
	getContextAttributes() { return { alpha: true, desynchronized: false, colorSpace: 'srgb', willReadFrequently: false }; }
	// transformaciones
	setTransform(a, b, c, d, e, f) { if (a && typeof a === 'object') ({ a, b, c, d, e, f } = a); else if (a === undefined) a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; this._setM([+a, +b, +c, +d, +e, +f]); }
	resetTransform() { this._setM([1, 0, 0, 1, 0, 0]); }
	getTransform() { const [a, b, c, d, e, f] = this._s.m; return G.DOMMatrix ? new G.DOMMatrix([a, b, c, d, e, f]) : { a, b, c, d, e, f, is2D: true, isIdentity: a === 1 && !b && !c && d === 1 && !e && !f }; }
	transform(a, b, c, d, e, f) { const m = this._s.m; this._setM([m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d, m[0] * e + m[2] * f + m[4], m[1] * e + m[3] * f + m[5]]); }
	translate(x, y) { this.transform(1, 0, 0, 1, +x || 0, +y || 0); }
	scale(x, y) { this.transform(+x, 0, 0, y === undefined ? +x : +y, 0, 0); }
	rotate(a) { const c = Math.cos(a), s = Math.sin(a); this.transform(c, s, -s, c, 0, 0); }
	// estilos
	get fillStyle() { return this._s.fillStyle; }
	set fillStyle(v) { this._paint('fill', v); }
	get strokeStyle() { return this._s.strokeStyle; }
	set strokeStyle(v) { this._paint('stroke', v); }
	_paint(k, v) {
		let c;
		if (v instanceof CanvasGradient || v instanceof CanvasPattern) c = v._flat(), this._s[k + 'Style'] = v;
		else { c = cssColor(v); if (!c) return; this._s[k + 'Style'] = colorStr(c); }
		this._s[k] = c; this._dirty = true;
	}
	createLinearGradient(x0, y0, x1, y1) { return new CanvasGradient(1, [x0, y0, x1, y1]); }
	createRadialGradient(x0, y0, r0, x1, y1, r1) { if (r0 < 0 || r1 < 0) throw new DOMException('radio negativo', 'IndexSizeError'); return new CanvasGradient(2, [x0, y0, r0, x1, y1, r1]); }
	createConicGradient(a, x, y) { return new CanvasGradient(3, [a, x, y]); }
	createPattern(img, rep) {
		const r = imgSource(img); if (!r) return null;
		const h = r[3] ? N.cvImgCanvas(r[0]) : r[0]; if (!h) return null;
		rep = rep == null || rep === '' ? 'repeat' : String(rep);
		return new CanvasPattern(h, rep === 'repeat' ? 3 : rep === 'repeat-x' ? 1 : rep === 'repeat-y' ? 2 : 0);
	}
	setLineDash(a) { a = Array.from(a || [], Number); if (a.some(v => !(v >= 0) || !isFinite(v))) return; this._s.lineDash = a; this._dirty = true; }
	getLineDash() { return this._s.lineDash.slice(); }
	// caminos
	beginPath() { N.cvBegin(this._id); }
	closePath() { N.cvClose(this._id); }
	moveTo(x, y) { N.cvMove(this._id, x, y); }
	lineTo(x, y) { N.cvLine(this._id, x, y); }
	bezierCurveTo(a, b, c, d, x, y) { N.cvBez(this._id, a, b, c, d, x, y); }
	quadraticCurveTo(a, b, x, y) { N.cvQuad(this._id, a, b, x, y); }
	arc(x, y, r, a0, a1, ccw) { if (r < 0) throw new DOMException('radio negativo', 'IndexSizeError'); N.cvEll(this._id, x, y, r, r, 0, a0, a1, !!ccw); }
	ellipse(x, y, rx, ry, rot, a0, a1, ccw) { N.cvEll(this._id, x, y, rx, ry, rot, a0, a1, !!ccw); }
	arcTo(x1, y1, x2, y2, r) { N.cvArcTo(this._id, x1, y1, x2, y2, r); }
	rect(x, y, w, h) { N.cvRect(this._id, x, y, w, h); }
	roundRect(x, y, w, h, r) {
		let rr = Array.isArray(r) ? r : [r === undefined ? 0 : r];
		rr = rr.map(v => typeof v === 'object' && v ? +v.x || 0 : +v || 0);
		const [tl, tr, br, bl] = rr.length === 1 ? [rr[0], rr[0], rr[0], rr[0]] : rr.length === 2 ? [rr[0], rr[1], rr[0], rr[1]] : rr.length === 3 ? [rr[0], rr[1], rr[2], rr[1]] : rr;
		const k = Math.min(1, Math.abs(w) / Math.max(1e-9, tl + tr, bl + br), Math.abs(h) / Math.max(1e-9, tl + bl, tr + br));
		this.moveTo(x + tl * k, y); this.arcTo(x + w, y, x + w, y + h, tr * k); this.arcTo(x + w, y + h, x, y + h, br * k);
		this.arcTo(x, y + h, x, y, bl * k); this.arcTo(x, y, x + w, y, tl * k); this.closePath();
	}
	_path(p) { if (!(p instanceof Path2D)) return false; this.beginPath(); for (const [k, a] of p._ops) this[k](...a); return true; }
	fill(p, rule) { if (typeof p === 'string') rule = p; else this._path(p); this._sync(); N.cvFill(this._id, rule === 'evenodd'); }
	stroke(p) { this._path(p); this._sync(); N.cvStroke(this._id); }
	clip(p, rule) { if (typeof p === 'string') rule = p; else this._path(p); N.cvClip(this._id, rule === 'evenodd'); }
	isPointInPath() { return false; }
	isPointInStroke() { return false; }
	fillRect(x, y, w, h) { this._sync(); N.cvFillRect(this._id, x, y, w, h); }
	strokeRect(x, y, w, h) { this.beginPath(); this.rect(x, y, w, h); this.stroke(); this.beginPath(); }
	clearRect(x, y, w, h) { N.cvClearRect(this._id, x, y, w, h); }
	// texto
	_text(t, x, y, draw, stroke) {
		const [px, bold] = fontOf(this._s.font);
		t = String(t).replace(/[\t\n\f\r]/g, ' ');
		const w = N.cvText(this._id, t, 0, 0, px, bold, false);
		if (!draw) return w;
		const al = this._s.textAlign, dir = this._s.direction === 'rtl';
		if (al === 'center') x -= w / 2; else if (al === 'right' || (al === 'end' && !dir) || (al === 'start' && dir)) x -= w;
		const [asc, desc] = N.cvMetrics(px, bold), bl = this._s.textBaseline;
		if (bl === 'top' || bl === 'hanging') y += asc; else if (bl === 'middle') y += (asc - desc) / 2; else if (bl === 'bottom' || bl === 'ideographic') y -= desc;
		if (stroke) { const f = this._s.fill; this._s.fill = this._s.stroke; this._dirty = true; this._sync(); this._s.fill = f; this._dirty = true; }
		else this._sync();
		N.cvText(this._id, t, x, y, px, bold, true);
		return w;
	}
	fillText(t, x, y) { this._text(t, x, y, true, false); }
	strokeText(t, x, y) { this._text(t, x, y, true, true); }
	measureText(t) {
		const [px, bold] = fontOf(this._s.font), w = this._text(t, 0, 0, false), [asc, desc] = N.cvMetrics(px, bold);
		return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: asc * 0.8, actualBoundingBoxDescent: desc * 0.6,
			fontBoundingBoxAscent: asc, fontBoundingBoxDescent: desc, emHeightAscent: asc, emHeightDescent: desc, alphabeticBaseline: 0, hangingBaseline: asc * 0.8, ideographicBaseline: -desc };
	}
	// imágenes y píxeles
	drawImage(img, a, b, c, d, e, f, g, h) {
		const r = imgSource(img);
		if (!r) return; // (vídeo, o una imagen que no se pudo bajar)
		const src = r[0], sw0 = r[1], sh0 = r[2];
		let sx = 0, sy = 0, sw = sw0, sh = sh0, dx, dy, dw, dh;
		if (e === undefined) { dx = a; dy = b; dw = c === undefined ? sw0 : c; dh = d === undefined ? sh0 : d; }
		else { sx = a; sy = b; sw = c; sh = d; dx = e; dy = f; dw = g; dh = h; }
		this._sync();
		if (r[3]) N.cvDrawCv(this._id, src, sx, sy, sw, sh, dx, dy, dw, dh); else N.cvDrawImg(this._id, src, sx, sy, sw, sh, dx, dy, dw, dh);
	}
	createImageData(w, h) { if (w instanceof ImageData) ({ width: w, height: h } = w); return new ImageData(Math.abs(w | 0), Math.abs(h | 0)); }
	getImageData(x, y, w, h) { return new ImageData(new Uint8ClampedArray(N.cvGet(this._id, x | 0, y | 0, w | 0, h | 0)), w | 0, h | 0); }
	putImageData(im, x, y) { N.cvPut(this._id, im.data, im.width, im.height, x | 0, y | 0); }
	drawFocusIfNeeded() {} scrollPathIntoView() {}
}
for (const k of ['globalAlpha', 'lineWidth', 'lineCap', 'lineJoin', 'miterLimit', 'globalCompositeOperation', 'font', 'textAlign', 'textBaseline', 'direction', 'shadowBlur', 'shadowColor',
	'shadowOffsetX', 'shadowOffsetY', 'imageSmoothingEnabled', 'imageSmoothingQuality', 'lineDashOffset', 'filter', 'letterSpacing', 'fontKerning'])
	Object.defineProperty(CanvasRenderingContext2D.prototype, k, {
		get() { return this._s[k]; },
		set(v) {
			if ((k === 'globalAlpha' && !(v >= 0 && v <= 1)) || ((k === 'lineWidth' || k === 'miterLimit') && !(v > 0))) return;
			this._s[k] = typeof this._s[k] === 'number' ? +v : typeof this._s[k] === 'boolean' ? !!v : String(v); this._dirty = true;
		}
	});
// del elemento <canvas>
function canvasCtx(el, type) {
	if (String(type).toLowerCase() !== '2d') return null; // (WebGL no)
	const id = el._cvid || (el._cvid = N.cvNew(el._h, el.width, el.height));
	if (!id) return null;
	let c = ctxs.get(id);
	if (!c) ctxs.set(id, c = new CanvasRenderingContext2D(el, id));
	return c;
}
function canvasResized(el) {
	const id = el._cvid; // (sin lienzo aún: ya se hará a su tamaño)
	if (!id) return;
	N.cvResize(id, el.width, el.height);
	const c = ctxs.get(id);
	if (c) c._reset();
}
G.CanvasRenderingContext2D = CanvasRenderingContext2D; G.CanvasGradient = CanvasGradient; G.CanvasPattern = CanvasPattern;
G.ImageData = ImageData; G.Path2D = Path2D;
// createImageBitmap: la misma imagen (o lienzo), ya lista
G.createImageBitmap = (src, ...a) => new Promise((ok, bad) => {
	setTimeout(() => {
		let r = src instanceof ImageData ? null : imgSource(src);
		if (src instanceof ImageData || (src && src.size !== undefined && src._t !== undefined)) { // (de ImageData: a un lienzo nuevo)
			if (!(src instanceof ImageData)) return bad(new DOMException('no se puede', 'InvalidStateError'));
			const cv = document.createElement('canvas'); cv.width = src.width; cv.height = src.height; cv.getContext('2d').putImageData(src, 0, 0); r = imgSource(cv);
		}
		if (!r) return bad(new DOMException('no se puede', 'InvalidStateError'));
		ok({ _bitmap: r, width: r[1], height: r[2], close() {} });
	}, 0);
});
G.ImageBitmap = class {};
G.OffscreenCanvas = class { constructor(w, h) { const e = document.createElement('canvas'); e.width = w; e.height = h; return e; } };
G.Image = class { constructor(w, h) { const e = document.createElement('img'); if (w) e.setAttribute('width', w); if (h) e.setAttribute('height', h); return e; } };
G.Option = class { constructor(t, v) { const e = document.createElement('option'); e.textContent = t || ''; if (v !== undefined) e.setAttribute('value', v); return e; } };
G.CSS = { supports: () => false, escape: s => String(s).replace(/[^\w-]/g, c => '\\' + c) };
G.DOMParser = class { parseFromString(s) { const f = document.createDocumentFragment(); for (const h of N.parse(String(s))) N.append(f._h, h); return { body: f, documentElement: f, querySelector: x => qs(f, x), querySelectorAll: x => qsa(f, x), getElementById: id => qs(f, '#' + id) }; } };
G.TextEncoder = class { encode(s) { const u = unescape(encodeURIComponent(String(s))); const a = new Uint8Array(u.length); for (let i = 0; i < u.length; i++) a[i] = u.charCodeAt(i); return a; } };
G.TextDecoder = class { decode(a) { if (!a) return ''; let s = ''; for (const b of new Uint8Array(a.buffer || a)) s += String.fromCharCode(b); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } } };
G.Blob = class { constructor(parts, o) { this._t = (parts || []).map(p => p && p._t != null ? p._t : typeof p === 'string' ? p : ArrayBuffer.isView(p) || p instanceof ArrayBuffer ? new G.TextDecoder().decode(p) : String(p)).join(''); this.size = this._t.length; this.type = (o && o.type) || ''; } text() { return Promise.resolve(this._t); } slice(a, b) { return new G.Blob([this._t.slice(a, b)], { type: this.type }); } arrayBuffer() { return Promise.resolve(new G.TextEncoder().encode(this._t).buffer); } };
G.File = class extends G.Blob { constructor(parts, name, o) { super(parts, o); this.name = name; this.lastModified = Date.now(); } };
G.FormData = class {
	constructor(form) { this._p = []; if (form) for (const e of form.querySelectorAll('input[name],select[name],textarea[name]')) { const t = (e.getAttribute('type') || '').toLowerCase(); if ((t === 'checkbox' || t === 'radio') && !e.checked) continue; if (t === 'submit' || t === 'button' || t === 'file' || e.disabled) continue; this._p.push([e.name, e.value]); } }
	_v(v, f) { return v && typeof v === 'object' && v._t != null ? (f ? Object.assign(Object.create(Object.getPrototypeOf(v)), v, { name: f }) : v) : String(v); }
	append(k, v, f) { this._p.push([String(k), this._v(v, f)]); }
	set(k, v, f) { this.delete(k); this.append(k, v, f); }
	get(k) { const e = this._p.find(x => x[0] === k); return e ? e[1] : null; }
	getAll(k) { return this._p.filter(x => x[0] === k).map(x => x[1]); }
	has(k) { return this._p.some(x => x[0] === k); }
	delete(k) { this._p = this._p.filter(x => x[0] !== k); }
	forEach(f, t) { for (const [k, v] of this._p) f.call(t, v, k, this); }
	entries() { return this._p[Symbol.iterator](); } keys() { return this._p.map(x => x[0])[Symbol.iterator](); } values() { return this._p.map(x => x[1])[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
};
class AbortSignal extends EventTarget {
	constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
	throwIfAborted() { if (this.aborted) throw this.reason; }
	_abort(r) { if (this.aborted) return; this.aborted = true; this.reason = r === undefined ? new Error('AbortError') : r; this.dispatchEvent(new Event('abort')); }
	static abort(r) { const s = new AbortSignal(); s._abort(r); return s; }
	static timeout(ms) { const s = new AbortSignal(); setTimeout(() => s._abort(new Error('TimeoutError')), ms); return s; }
	static any(list) { const s = new AbortSignal(); for (const x of list) { if (x.aborted) { s._abort(x.reason); break; } x.addEventListener('abort', () => s._abort(x.reason)); } return s; }
}
G.AbortSignal = AbortSignal;
G.AbortController = class { constructor() { this.signal = new AbortSignal(); } abort(r) { this.signal._abort(r); } };
G.WebSocket = class { constructor() { setTimeout(() => this.onerror && this.onerror(new Event('error')), 0); } send() {} close() {} };
G.Worker = class { postMessage() {} terminate() {} addEventListener() {} };
G.BroadcastChannel = class { postMessage() {} close() {} addEventListener() {} };

/* ---------- mensajes entre ventanas y canales (React los usa para repartir su trabajo): llegan después,
 * como en un navegador, con su MessageEvent */
G.origin = location.origin;
G.name = '';
G.closed = false; G.opener = null; G.frameElement = null; G.length = 0;
G.isSecureContext = location.protocol === 'https:';
G.postMessage = (data, targetOrigin) => { setTimeout(() => G.dispatchEvent(new G.MessageEvent('message', { data, origin: location.origin, source: G })), 0); };
/* las otras ventanas (la página o sus iframes): solo se les puede mandar mensajes, como entre orígenes distintos. El
 * mismo objeto para la misma ventana (event.source === iframe.contentWindow) */
const FRAME = N.frame || 0, winProxies = new Map();
function winProxy(id) {
	let w = winProxies.get(id); if (w) return w;
	w = { postMessage(d, o) { let j; try { j = JSON.stringify(d === undefined ? null : d); } catch (e) { j = 'null'; } N.post(id, j, location.origin); },
		closed: false, length: 0, focus() {}, blur() {}, close() {} };
	w.window = w.self = w.frames = w; w.top = id === 0 ? w : winProxy(0); w.parent = w.top;
	Object.defineProperty(w, 'location', { get() { throw new DOMException('Blocked a frame from accessing a cross-origin frame.', 'SecurityError'); } });
	winProxies.set(id, w); return w;
}
if (FRAME) { G.parent = G.top = winProxy(0); G.frameElement = null; }
// un mensaje de otra ventana (lo trae el navegador): su evento «message»
G.__message = (json, origin, from) => { let d; try { d = JSON.parse(json); } catch (e) { d = json; } G.dispatchEvent(new G.MessageEvent('message', { data: d, origin, source: winProxy(from), ports: [] })); };
if (!G.DOMException) G.DOMException = class extends Error { constructor(m, n) { super(m); this.name = n || 'Error'; } };
class MessagePort extends EventTarget {
	constructor() { super(); this._other = null; this._started = false; this._q = []; }
	postMessage(data) { const o = this._other; if (!o) return; setTimeout(() => { const ev = new G.MessageEvent('message', { data, target: o }); if (typeof o.onmessage === 'function') try { o.onmessage(ev); } catch (e) { report(e); } o.dispatchEvent(ev); }, 0); }
	start() { this._started = true; } close() { this._other = null; }
}
G.MessagePort = MessagePort;
G.MessageChannel = class { constructor() { this.port1 = new MessagePort(); this.port2 = new MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; } };
// copia profunda (lo que cabe en JSON, más fechas, Map y Set)
G.structuredClone = (v, seen = new Map()) => {
	if (v === null || typeof v !== 'object') return v;
	if (seen.has(v)) return seen.get(v);
	if (v instanceof Date) return new Date(v.getTime());
	if (v instanceof RegExp) return new RegExp(v.source, v.flags);
	if (ArrayBuffer.isView(v)) return v.slice();
	if (v instanceof Map) { const m = new Map(); seen.set(v, m); for (const [k, x] of v) m.set(G.structuredClone(k, seen), G.structuredClone(x, seen)); return m; }
	if (v instanceof Set) { const m = new Set(); seen.set(v, m); for (const x of v) m.add(G.structuredClone(x, seen)); return m; }
	const o = Array.isArray(v) ? [] : {}; seen.set(v, o);
	for (const k of Object.keys(v)) o[k] = G.structuredClone(v[k], seen);
	return o;
};
G.FileReader = class extends EventTarget {
	constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
	_done(r) { this.readyState = 2; this.result = r; setTimeout(() => { const ev = new Event('load'); for (const t of ['load', 'loadend']) { const e = new Event(t); e.target = this; if (typeof this['on' + t] === 'function') try { this['on' + t](e); } catch (x) { report(x); } this.dispatchEvent(e); } }, 0); }
	readAsText(b) { this._done(b && b._t != null ? b._t : String(b)); }
	readAsDataURL(b) { this._done('data:' + (b && b.type || 'application/octet-stream') + ';base64,' + G.btoa(unescape(encodeURIComponent(b && b._t != null ? b._t : '')))); }
	readAsArrayBuffer(b) { this._done(new G.TextEncoder().encode(b && b._t != null ? b._t : '').buffer); }
	readAsBinaryString(b) { this._done(b && b._t != null ? b._t : ''); }
	abort() {}
};
G.XMLSerializer = class { serializeToString(n) { return n.nodeType === ELEMENT ? n.outerHTML : n.nodeType === DOC ? n.documentElement.outerHTML : n.textContent; } };
G.clearImmediate = id => clearTimeout(id);
G.PromiseRejectionEvent = class extends Event {};
G.Window = function Window() {}; G.Window.prototype = Object.getPrototypeOf(G);
G.NodeList = NodeList; G.HTMLCollection = HTMLCollection; G.DOMTokenList = DOMTokenList; G.DOMRect = DOMRect; G.DOMRectReadOnly = DOMRectReadOnly;
// (las listas del DOM que algunas librerías recorren al empezar: core-js, por ejemplo)
for (const k of ['CSSRuleList', 'CSSStyleDeclaration', 'CSSValueList', 'ClientRectList', 'DOMRectList', 'DOMStringList', 'DataTransferItemList', 'FileList', 'HTMLAllCollection', 'MediaList', 'MimeTypeArray', 'NamedNodeMap',
	'PaintRequestList', 'Plugin', 'PluginArray', 'SVGLengthList', 'SVGNumberList', 'SVGPathSegList', 'SVGPointList', 'SVGStringList', 'SVGTransformList', 'SourceBufferList', 'StyleSheetList', 'TextTrackCueList', 'TextTrackList', 'TouchList'])
	if (!G[k]) G[k] = class extends Array { item(i) { return this[i] ?? null; } };
G.Storage = class {};
// rangos y selección: lo básico (sin medidas de verdad)
class Range {
	constructor() { this.startContainer = document; this.startOffset = 0; this.endContainer = document; this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = document; }
	setStart(n, o) { this.startContainer = n; this.startOffset = o; } setEnd(n, o) { this.endContainer = n; this.endOffset = o; this.collapsed = false; }
	setStartBefore(n) { this.startContainer = n.parentNode; } setStartAfter(n) { this.startContainer = n.parentNode; } setEndBefore(n) { this.endContainer = n.parentNode; } setEndAfter(n) { this.endContainer = n.parentNode; }
	selectNode(n) { this.startContainer = this.endContainer = this.commonAncestorContainer = n.parentNode || n; } selectNodeContents(n) { this.startContainer = this.endContainer = this.commonAncestorContainer = n; }
	collapse() { this.collapsed = true; } cloneRange() { return Object.assign(new Range(), this); } detach() {}
	createContextualFragment(h) { const f = document.createDocumentFragment(); for (const x of N.parse(String(h))) N.append(f._h, x); return f; }
	getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; } getClientRects() { return []; }
	toString() { return ''; } deleteContents() {} extractContents() { return document.createDocumentFragment(); } cloneContents() { return document.createDocumentFragment(); } insertNode() {} surroundContents() {}
}
G.Range = Range;
const selection = { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null, getRangeAt: () => new Range(), addRange() {}, removeAllRanges() {}, removeRange() {}, collapse() {}, selectAllChildren() {}, toString: () => '', empty() {} };
G.getSelection = () => selection;
G.speechSynthesis = undefined;

/* ---------- red: fetch y XMLHttpRequest (las respuestas llegan sin esperar: va en su hilo). Las cabeceras que
 * pide la página van de verdad (una API que espera JSON lo dice en Content-Type), y la respuesta trae su tipo */
class Headers {
	constructor(init) {
		this._m = new Map();
		if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
		else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
		else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
	}
	append(k, v) { k = String(k).toLowerCase(); const o = this._m.get(k); this._m.set(k, o == null ? String(v) : o + ', ' + v); }
	set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
	get(k) { const v = this._m.get(String(k).toLowerCase()); return v == null ? null : v; }
	has(k) { return this._m.has(String(k).toLowerCase()); }
	delete(k) { this._m.delete(String(k).toLowerCase()); }
	forEach(f, t) { for (const [k, v] of this._m) f.call(t, v, k, this); }
	entries() { return this._m.entries(); } keys() { return this._m.keys(); } values() { return this._m.values(); }
	[Symbol.iterator]() { return this._m.entries(); }
	_lines() { let s = ''; for (const [k, v] of this._m) s += k + ': ' + v + '\n'; return s; }
}
G.Headers = Headers;
function bodyStr(b) {
	if (b == null) return null;
	if (typeof b === 'string') return b;
	if (b instanceof URLSearchParams) return b.toString();
	if (b instanceof G.FormData) return b._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
	if (b instanceof G.Blob) return b._t;
	if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return new G.TextDecoder().decode(b);
	return String(b);
}
// FormData como lo manda un navegador: multipart/form-data con su separador (y los archivos con su nombre y tipo)
function multipart(fd) {
	const bnd = '----Cloud3DSFormBoundary' + Math.random().toString(36).slice(2, 12);
	let out = '';
	for (const [k, v] of fd._p) {
		out += '--' + bnd + '\r\n';
		if (v && typeof v === 'object' && v._t != null) out += 'Content-Disposition: form-data; name="' + k + '"; filename="' + (v.name || 'blob') + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n' + v._t + '\r\n';
		else out += 'Content-Disposition: form-data; name="' + k + '"\r\n\r\n' + v + '\r\n';
	}
	return [out + '--' + bnd + '--\r\n', 'multipart/form-data; boundary=' + bnd];
}
function doFetch(url, method, body, headers) {
	const h = headers instanceof Headers ? headers : new Headers(headers);
	if (body instanceof URLSearchParams && !h.has('content-type')) h.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');
	if (body instanceof G.FormData) { const [b, t] = multipart(body); body = b; h.set('content-type', t); }
	else if (body instanceof G.Blob && body.type && !h.has('content-type')) h.set('content-type', body.type);
	else if (typeof body === 'string' && method && String(method).toUpperCase() !== 'GET' && !h.has('content-type')) h.set('content-type', 'text/plain;charset=UTF-8');
	const r = N.fetch(resolveUrl(location.href, String(url)), String(method || 'GET').toUpperCase(), bodyStr(body), h._lines());
	if (!r) throw new TypeError('Failed to fetch');
	return { status: r[0], text: r[1], url: r[2], type: r[3] || '' };
}
class Response {
	constructor(r, init) {
		if (typeof r === 'string' || r == null) r = { status: init && init.status || 200, text: r == null ? '' : r, url: '', type: init && init.headers ? new Headers(init.headers).get('content-type') || '' : '' };
		this._r = r; this.status = r.status; this.ok = r.status >= 200 && r.status < 300; this.url = r.url; this.statusText = r.status === 200 ? 'OK' : ''; this.redirected = false;
		this.type = 'basic'; this.bodyUsed = false;
		this.headers = new Headers(r.type ? { 'content-type': r.type } : {});
	}
	text() { this.bodyUsed = true; return Promise.resolve(this._r.text); }
	json() { this.bodyUsed = true; try { return Promise.resolve(JSON.parse(this._r.text)); } catch (e) { return Promise.reject(e); } }
	blob() { return Promise.resolve(new Blob([this._r.text], { type: this._r.type })); }
	arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._r.text).buffer); }
	formData() { const f = new G.FormData(); for (const [k, v] of new URLSearchParams(this._r.text)) f.append(k, v); return Promise.resolve(f); }
	clone() { return new Response(this._r); }
	static json(d, i) { return new Response(JSON.stringify(d), Object.assign({ headers: { 'content-type': 'application/json' } }, i)); }
	static error() { return new Response({ status: 0, text: '', url: '', type: '' }); }
}
G.Response = Response;
G.Request = class { constructor(u, o) { o = o || {}; if (u && u.url) { o = Object.assign({ method: u.method, headers: u.headers, body: u.body }, o); u = u.url; } this.url = resolveUrl(location.href, String(u)); this.method = String(o.method || 'GET').toUpperCase(); this.headers = new Headers(o.headers); this.body = o.body; this.signal = o.signal; this.credentials = o.credentials || 'same-origin'; } clone() { return new G.Request(this.url, this); } };
G.fetch = (u, o) => {
	o = o || {};
	try {
		let method = o.method, headers = o.headers, body = o.body;
		if (u && typeof u === 'object' && u.url) { method = method || u.method; headers = headers || u.headers; body = body !== undefined ? body : u.body; u = u.url; }
		if (o.signal && o.signal.aborted) return Promise.reject(o.signal.reason || new Error('AbortError'));
		return Promise.resolve(new Response(doFetch(u, method, body, headers)));
	} catch (e) { return Promise.reject(e); }
};
G.XMLHttpRequest = class extends EventTarget {
	constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget(); this._rq = new Headers(); this._type = ''; }
	open(m, u) { this._m = m; this._u = u; this.readyState = 1; this._rq = new Headers(); }
	setRequestHeader(k, v) { this._rq.append(k, v); }
	getResponseHeader(k) { return String(k).toLowerCase() === 'content-type' && this._type ? this._type : null; }
	getAllResponseHeaders() { return this._type ? 'content-type: ' + this._type + '\r\n' : ''; }
	overrideMimeType() {} abort() { this._aborted = true; }
	send(body) {
		setTimeout(() => {
			if (this._aborted) return;
			try { const r = doFetch(this._u, this._m, body, this._rq); this.status = r.status; this.statusText = r.status === 200 ? 'OK' : ''; this.responseText = r.text; this.responseURL = r.url; this._type = r.type;
				this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(r.text); } catch (e) { return null; } })() : this.responseType === 'arraybuffer' ? new TextEncoder().encode(r.text).buffer : r.text; }
			catch (e) { this.status = 0; }
			this.readyState = 4;
			const fire = t => { const ev = new Event(t); ev.target = this; if (typeof this['on' + t] === 'function') try { this['on' + t](ev); } catch (e) { report(e); } this.dispatchEvent(ev); };
			fire('readystatechange'); fire(this.status ? 'load' : 'error'); fire('loadend');
		}, 0);
	}
};
G.XMLHttpRequest.UNSENT = 0; G.XMLHttpRequest.OPENED = 1; G.XMLHttpRequest.HEADERS_RECEIVED = 2; G.XMLHttpRequest.LOADING = 3; G.XMLHttpRequest.DONE = 4;

/* ---------- temporizadores: corren con un reloj propio, sin esperar de verdad */
let tid = 0, vnow = 0; const timers = new Map();
G.setTimeout = (f, ms, ...a) => { const id = ++tid; timers.set(id, { f, at: vnow + Math.max(0, +ms || 0), a, every: 0 }); return id; };
G.setInterval = (f, ms, ...a) => { const id = ++tid; const e = Math.max(10, +ms || 0); timers.set(id, { f, at: vnow + e, a, every: e }); return id; };
G.clearTimeout = G.clearInterval = id => timers.delete(id);
G.requestAnimationFrame = f => setTimeout(() => f(N.now()), 16);
G.cancelAnimationFrame = id => clearTimeout(id);
G.queueMicrotask = f => Promise.resolve().then(f);
G.setImmediate = f => setTimeout(f, 0);
G.__pending = () => { for (const t of timers.values()) if (!t.every) return true; return false; }; // queda algo que no sea un intervalo
G.__timers = delta => {
	const limit = vnow + delta, t0 = N.now();
	let ran = 0;
	for (let guard = 0; guard < 2000 && N.now() - t0 < 400; guard++) { // a ratos: entre medias se ve la página
		let best = null, bid = 0;
		for (const [id, t] of timers) if (!best || t.at < best.at) { best = t; bid = id; }
		if (!best || best.at > limit) break;
		vnow = Math.max(vnow, best.at);
		if (best.every) best.at += best.every; else timers.delete(bid);
		try { typeof best.f === 'function' ? best.f(...best.a) : (0, eval)(String(best.f)); } catch (e) { report(e); }
		ran++;
		if (best.every && ran > 200) break;
	}
	return [...timers.values()].some(t => t.at <= limit && !t.every);
};
G.__click = (h, x, y) => {
	const el = W(h); if (!el) return true;
	// dónde: en la página (pageX), en la ventana (clientX, con el scroll) y en el elemento (offsetX); como un toque
	// de verdad, antes su pointerdown/mousedown/touchstart y sus «up» (los juegos y los menús escuchan esos)
	const at = {};
	if (x >= 0 && y >= 0) {
		const r = el.getBoundingClientRect(), sx = G.scrollX || 0, sy = G.scrollY || 0;
		Object.assign(at, { pageX: x, pageY: y, clientX: x - sx, clientY: y - sy, x: x - sx, y: y - sy, screenX: x - sx, screenY: y - sy, offsetX: x - sx - r.left, offsetY: y - sy - r.top });
	}
	const mk = (C, t, o) => { const e = new C(t, { bubbles: true, cancelable: true }); Object.assign(e, at, { button: 0, which: 1, buttons: t.endsWith('down') ? 1 : 0, pointerId: 1, pointerType: 'touch', isPrimary: true }, o); return e; };
	if (x >= 0) {
		const touch = { identifier: 0, target: el, ...at };
		for (const [C, t] of [[G.PointerEvent, 'pointerdown'], [G.TouchEvent, 'touchstart'], [G.MouseEvent, 'mousedown'], [G.PointerEvent, 'pointerup'], [G.TouchEvent, 'touchend'], [G.MouseEvent, 'mouseup']])
			try { el.dispatchEvent(mk(C, t, C === G.TouchEvent ? { touches: t === 'touchstart' ? [touch] : [], changedTouches: [touch], targetTouches: t === 'touchstart' ? [touch] : [] } : {})); } catch (e) { report(e); }
	}
	const ev = mk(G.MouseEvent, 'click', {});
	el.dispatchEvent(ev);
	// <label for> y casillas: como un navegador
	if (!ev.defaultPrevented && el.localName === 'label') { const f = el.getAttribute('for'); const t = f ? document.getElementById(f) : el.querySelector('input'); if (t && t !== el) t.click(); }
	return !ev.defaultPrevented;
};
// lo que se escribió en una caja de texto (con el teclado de la consola): su valor y los eventos de escribir; con
// enter, como pulsar Intro (y el submit de su formulario). Devuelve false si la página lo canceló (no se envía)
G.__input = (h, v, enter) => {
	const el = W(h); if (!el) return true;
	const fire = (t, o) => { const ev = new (t.startsWith('key') ? G.KeyboardEvent : t === 'input' ? G.InputEvent : Event)(t, { bubbles: true, cancelable: true, ...o }); Object.assign(ev, o); el.dispatchEvent(ev); return ev; };
	try { el.dispatchEvent(new G.FocusEvent('focus')); el.dispatchEvent(new G.FocusEvent('focusin', { bubbles: true })); } catch (e) { report(e); }
	el.value = v;
	const last = v.slice(-1);
	if (last) { fire('keydown', { key: last, code: '', keyCode: last.charCodeAt(0), which: last.charCodeAt(0) }); fire('keyup', { key: last, keyCode: last.charCodeAt(0), which: last.charCodeAt(0) }); }
	fire('input', { data: v, inputType: 'insertText' });
	fire('change', {});
	if (!enter) return true;
	const kd = fire('keydown', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13 });
	fire('keypress', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, charCode: 13 });
	fire('keyup', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13 });
	if (kd.defaultPrevented) return false;
	const f = el.closest && el.closest('form');
	if (f) { const se = new Event('submit', { bubbles: true, cancelable: true }); se.submitter = null; f.dispatchEvent(se); if (se.defaultPrevented) return false; }
	return true;
};
G.__current = s => { G.__script = s ? W(s._h || s) : null; if (G.__script) ran.add(N.id(G.__script._h)); };
G.__loaded = () => {
	G.__ready = true;
	document.dispatchEvent(new Event('readystatechange'));
	document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
	G.dispatchEvent(new Event('load'));
	if (typeof G.onload === 'function') try { G.onload(new Event('load')); } catch (e) { report(e); }
	G.dispatchEvent(new Event('pageshow'));
};
function navigateForm(form) {
	const fd = new G.FormData(form);
	const q = fd._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
	const act = resolveUrl(location.href, form.getAttribute('action') || location.href).replace(/[?#].*$/, '');
	N.navigate(act + '?' + q);
}
// Intl: QuickJS no lo trae y muchas webs (DuckDuckGo, las hechas con Next.js…) lo usan nada más
// empezar; esto es lo básico (números, fechas, plurales, comparar, listas, tiempo relativo)
if (typeof G.Intl === 'undefined') {
	const loc = l => (Array.isArray(l) ? l[0] : l) || 'es-ES';
	const sep = l => /^(es|de|it|pt|nl|fr|ru|pl|tr|id|da|sv|fi|nb|cs)/i.test(l) ? ['.', ','] : [',', '.'];
	const sup = l => l == null ? [] : Array.isArray(l) ? l.slice() : [l];
	const group = (s, g) => s.replace(/\B(?=(\d{3})+(?!\d))/g, g);
	const pad = (n, k) => String(n).padStart(k, '0');
	const MES = ['enero', 'febrero', 'marzo', 'abril', 'mayo', 'junio', 'julio', 'agosto', 'septiembre', 'octubre', 'noviembre', 'diciembre'];
	const MON = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
	const DIA = ['domingo', 'lunes', 'martes', 'miércoles', 'jueves', 'viernes', 'sábado'];
	const DAY = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
	class NumberFormat {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; this.format = this.format.bind(this); }
		format(n) {
			const o = this.o, [g, d] = sep(this.l);
			n = Number(n);
			if (!isFinite(n)) return isNaN(n) ? 'NaN' : (n < 0 ? '-∞' : '∞');
			if (o.style === 'percent') n *= 100;
			if (o.notation === 'compact') {
				const a = Math.abs(n), u = a >= 1e9 ? [1e9, ' mil M'] : a >= 1e6 ? [1e6, ' M'] : a >= 1e3 ? [1e3, ' mil'] : null;
				if (u) return (Math.round(n / u[0] * 10) / 10).toString().replace('.', d) + u[1];
			}
			const mx = o.maximumFractionDigits != null ? o.maximumFractionDigits : (o.style === 'currency' ? 2 : o.style === 'percent' ? 0 : 3);
			const mn = o.minimumFractionDigits != null ? o.minimumFractionDigits : (o.style === 'currency' ? 2 : 0);
			let t = Math.abs(n).toFixed(Math.max(mn, Math.min(mx, 20)));
			if (t.indexOf('.') >= 0) { t = t.replace(/0+$/, ''); const f = t.split('.')[1] || ''; if (f.length < mn) t += '0'.repeat(mn - f.length); t = t.replace(/\.$/, ''); }
			let [i, f] = t.split('.');
			if (o.useGrouping !== false && !(d === ',' && i.length <= 4 && /^es/i.test(this.l))) i = group(i, g);
			let r = (n < 0 ? '-' : '') + i + (f ? d + f : '');
			if (o.style === 'percent') r += d === ',' ? ' %' : '%';
			if (o.style === 'currency') { const c = { EUR: '€', USD: '$', GBP: '£', JPY: '¥' }[o.currency] || o.currency || ''; r = d === ',' ? r + ' ' + c : c + r; }
			if (o.style === 'unit' && o.unit) r += ' ' + o.unit;
			return r;
		}
		formatToParts(n) { return [{ type: 'integer', value: this.format(n) }]; }
		resolvedOptions() { return Object.assign({ locale: this.l, numberingSystem: 'latn', style: 'decimal', minimumFractionDigits: 0, maximumFractionDigits: 3 }, this.o); }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class DateTimeFormat {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; this.format = this.format.bind(this); }
		format(x) {
			const t = x == null ? new Date() : new Date(x), o = this.o, es = /^es/i.test(this.l);
			if (isNaN(t)) return 'Invalid Date';
			const parts = [], has = k => o[k] !== undefined;
			const any = ['year', 'month', 'day', 'weekday', 'hour', 'minute', 'second'].some(has) || o.dateStyle || o.timeStyle;
			const dY = !any || has('year') || o.dateStyle, dM = !any || has('month') || o.dateStyle, dD = !any || has('day') || o.dateStyle;
			const long = o.month === 'long' || o.dateStyle === 'long' || o.dateStyle === 'full', short = o.month === 'short' || o.dateStyle === 'medium';
			if (has('weekday') || o.dateStyle === 'full') parts.push((es ? DIA : DAY)[t.getDay()] + ',');
			if (dD || dM || dY) {
				if (long || short) {
					let m = (es ? MES : MON)[t.getMonth()]; if (short) m = m.slice(0, 3);
					parts.push(es ? [dD ? t.getDate() : '', dM ? 'de ' + m : '', dY ? 'de ' + t.getFullYear() : ''].filter(Boolean).join(' ') : [dM ? m : '', dD ? t.getDate() + ',' : '', dY ? t.getFullYear() : ''].filter(Boolean).join(' '));
				} else {
					const a = [dD && (o.day === '2-digit' ? pad(t.getDate(), 2) : t.getDate()), dM && (o.month === '2-digit' ? pad(t.getMonth() + 1, 2) : t.getMonth() + 1), dY && t.getFullYear()].filter(v => v !== false && v !== undefined);
					parts.push(es ? a.join('/') : [a[1], a[0], a[2]].filter(v => v !== undefined).join('/'));
				}
			}
			if (has('hour') || has('minute') || o.timeStyle) parts.push(pad(t.getHours(), has('hour') && o.hour !== '2-digit' ? 1 : 2) + ':' + pad(t.getMinutes(), 2) + (has('second') || o.timeStyle === 'medium' ? ':' + pad(t.getSeconds(), 2) : ''));
			return parts.join(' ').replace(/,$/, '');
		}
		formatToParts(x) { return [{ type: 'literal', value: this.format(x) }]; }
		formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
		resolvedOptions() { return Object.assign({ locale: this.l, calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }, this.o); }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class PluralRules {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; }
		select(n) { return this.o.type === 'ordinal' ? 'other' : Math.abs(Number(n)) === 1 ? 'one' : 'other'; }
		resolvedOptions() { return { locale: this.l, type: this.o.type || 'cardinal', pluralCategories: ['one', 'other'] }; }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class Collator {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; this.compare = this.compare.bind(this); } // como en los navegadores: se puede pasar suelta a sort
		compare(a, b) {
			a = String(a); b = String(b);
			if (this.o.sensitivity === 'base' || this.o.sensitivity === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); }
			if (this.o.numeric) { const x = parseFloat(a), y = parseFloat(b); if (!isNaN(x) && !isNaN(y) && x !== y) return x < y ? -1 : 1; }
			const na = a.normalize ? a.normalize('NFD').replace(/[̀-ͯ]/g, '') : a, nb = b.normalize ? b.normalize('NFD').replace(/[̀-ͯ]/g, '') : b;
			return na < nb ? -1 : na > nb ? 1 : a < b ? -1 : a > b ? 1 : 0;
		}
		resolvedOptions() { return Object.assign({ locale: this.l, usage: 'sort', sensitivity: 'variant' }, this.o); }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class RelativeTimeFormat {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; }
		format(v, u) {
			u = String(u).replace(/s$/, ''); const n = Math.abs(v), es = /^es/i.test(this.l);
			const W = es ? { second: ['segundo', 'segundos'], minute: ['minuto', 'minutos'], hour: ['hora', 'horas'], day: ['día', 'días'], week: ['semana', 'semanas'], month: ['mes', 'meses'], year: ['año', 'años'] } : null;
			const w = W ? (W[u] || [u, u + 's'])[n === 1 ? 0 : 1] : (n === 1 ? u : u + 's');
			return es ? (v < 0 ? 'hace ' + n + ' ' + w : 'dentro de ' + n + ' ' + w) : (v < 0 ? n + ' ' + w + ' ago' : 'in ' + n + ' ' + w);
		}
		formatToParts(v, u) { return [{ type: 'literal', value: this.format(v, u) }]; }
		resolvedOptions() { return { locale: this.l, style: 'long', numeric: 'always' }; }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class ListFormat {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; }
		format(list) {
			const a = Array.from(list).map(String), es = /^es/i.test(this.l), y = this.o.type === 'disjunction' ? (es ? ' o ' : ' or ') : (es ? ' y ' : ' and ');
			return a.length < 2 ? (a[0] || '') : a.slice(0, -1).join(', ') + y + a[a.length - 1];
		}
		formatToParts(list) { return [{ type: 'literal', value: this.format(list) }]; }
		static supportedLocalesOf(l) { return sup(l); }
	}
	class Segmenter {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; }
		segment(str) {
			str = String(str); const w = this.o.granularity === 'word', out = [];
			if (w) { const re = /\w+|\W+/gu; let m; while ((m = re.exec(str))) out.push({ segment: m[0], index: m.index, input: str, isWordLike: /\w/.test(m[0]) }); }
			else { let i = 0; for (const c of str) { out.push({ segment: c, index: i, input: str }); i += c.length; } }
			out.containing = k => out.find(s => k >= s.index && k < s.index + s.segment.length);
			return out;
		}
		static supportedLocalesOf(l) { return sup(l); }
	}
	class DisplayNames {
		constructor(l, o) { this.l = loc(l); this.o = o || {}; }
		of(c) { return c; }
		static supportedLocalesOf(l) { return sup(l); }
	}
	G.Intl = { NumberFormat, DateTimeFormat, PluralRules, Collator, RelativeTimeFormat, ListFormat, Segmenter, DisplayNames, Locale: class { constructor(t) { this.baseName = String(t); this.language = String(t).split('-')[0]; } toString() { return this.baseName; } maximize() { return this; } minimize() { return this; } },
		getCanonicalLocales: l => sup(l), supportedValuesOf: () => [] };
	if (!Number.prototype.toLocaleString || (1234.5).toLocaleString() === '1234.5') Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this); };
	const dtl = Date.prototype.toLocaleDateString;
	Date.prototype.toLocaleDateString = function (l, o) { try { return new DateTimeFormat(l, o || { year: 'numeric', month: 'numeric', day: 'numeric' }).format(this); } catch (e) { return dtl.call(this); } };
	Date.prototype.toLocaleTimeString = function (l, o) { return new DateTimeFormat(l, o || { hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(this); };
	Date.prototype.toLocaleString = function (l, o) { return new DateTimeFormat(l, o || { year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(this); };
	String.prototype.localeCompare = function (b, l, o) { return new Collator(l, o).compare(this, b); };
}
})(globalThis);
