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

/* ---------- eventos */
class Event {
	constructor(type, init) {
		init = init || {};
		this.type = type; this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable;
		this.defaultPrevented = false; this._stop = false; this.target = null; this.currentTarget = null;
		this.timeStamp = N.now(); this.detail = init.detail;
	}
	preventDefault() { this.defaultPrevented = true; }
	stopPropagation() { this._stop = true; }
	stopImmediatePropagation() { this._stop = true; }
}
class CustomEvent extends Event {}
class EventTarget {
	addEventListener(type, fn, opt) {
		if (!fn) return;
		const id = this._h ? N.id(this._h) : this === G ? 'window' : 'other';
		let m = listeners.get(id); if (!m) listeners.set(id, m = {});
		(m[type] = m[type] || []).push(fn);
	}
	removeEventListener(type, fn) {
		const id = this._h ? N.id(this._h) : this === G ? 'window' : 'other';
		const m = listeners.get(id); if (!m || !m[type]) return;
		m[type] = m[type].filter(f => f !== fn);
	}
	dispatchEvent(ev) {
		if (!ev.target) ev.target = this;
		let node = this;
		while (node) {
			const id = node._h ? N.id(node._h) : node === G ? 'window' : 'other';
			const m = listeners.get(id);
			ev.currentTarget = node;
			let on = node['on' + ev.type];
			if (typeof on !== 'function' && node._h && N.type(node._h) === ELEMENT) { // onclick="…" en el HTML
				const code = N.getA(node._h, 'on' + ev.type);
				if (code) try { on = inlineHandler(node, ev.type, code); } catch (e) { report(e); }
			}
			if (typeof on === 'function') try { if (on.call(node, ev) === false) ev.preventDefault(); } catch (e) { report(e); }
			if (m && m[ev.type]) for (const f of m[ev.type].slice()) {
				try { typeof f === 'function' ? f.call(node, ev) : f.handleEvent(ev); } catch (e) { report(e); }
				if (ev._stop) break;
			}
			if (!ev.bubbles || ev._stop) break;
			node = node === document ? G : node.parentNode || (node === G ? null : null);
		}
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
	get childNodes() { const a = []; for (let c = N.first(this._h); c; c = N.next(c)) a.push(W(c)); a.item = i => a[i] || null; return a; }
	get ownerDocument() { return document; }
	get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === document; }
	get textContent() { return N.text(this._h); }
	set textContent(v) { N.setText(this._h, v == null ? '' : String(v)); }
	get nodeValue() { return this.nodeType === ELEMENT ? null : N.data(this._h); }
	set nodeValue(v) { if (this.nodeType !== ELEMENT) N.setData(this._h, String(v)); }
	hasChildNodes() { return !!N.first(this._h); }
	appendChild(c) { c = nodeOf(c); if (c instanceof DocumentFragment) { for (const k of c.childNodes) N.append(this._h, k._h); return c; } N.append(this._h, c._h); return c; }
	insertBefore(c, ref) { c = nodeOf(c); if (c instanceof DocumentFragment) { for (const k of c.childNodes) N.insert(this._h, k._h, H(ref)); return c; } N.insert(this._h, c._h, H(ref)); return c; }
	removeChild(c) { N.removeChild(this._h, c._h); return c; }
	replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
	cloneNode(deep) { return W(N.clone(this._h, !!deep)); }
	contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
	remove() { const p = this.parentNode; if (p) p.removeChild(this); }
	compareDocumentPosition() { return 0; }
	getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
	isEqualNode(o) { return o === this; }
	isSameNode(o) { return o === this; }
}
class CharacterData extends Node {
	get data() { return N.data(this._h); }
	set data(v) { N.setData(this._h, String(v)); }
	get length() { return this.data.length; }
	get nextElementSibling() { return nextEl(this); }
	get previousElementSibling() { return prevEl(this); }
}
class Text extends CharacterData { get wholeText() { return this.data; } }
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
function qsa(root, sel) { const out = []; walkEls(root, e => { if (matches(e, sel)) out.push(e); }); out.item = i => out[i] || null; return out; }
function qs(root, sel) { let r = null; walkEls(root, e => { if (matches(e, sel)) { r = e; return true; } }); return r; }

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
	setAttribute(n, v) { N.setA(this._h, String(n).toLowerCase(), String(v)); }
	removeAttribute(n) { N.remA(this._h, String(n).toLowerCase()); }
	hasAttribute(n) { return N.getA(this._h, String(n).toLowerCase()) !== null; }
	toggleAttribute(n, f) { const h = this.hasAttribute(n); const want = f === undefined ? !h : !!f; if (want) this.setAttribute(n, ''); else this.removeAttribute(n); return want; }
	getAttributeNames() { return N.attrs(this._h).map(x => x[0]); }
	get children() { const a = []; for (let c = N.first(this._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) a.push(W(c)); a.item = i => a[i] || null; return a; }
	get childElementCount() { return this.children.length; }
	get firstElementChild() { for (let c = N.first(this._h); c; c = N.next(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
	get lastElementChild() { for (let c = N.last(this._h); c; c = N.prev(c)) if (N.type(c) === ELEMENT) return W(c); return null; }
	get nextElementSibling() { return nextEl(this); }
	get previousElementSibling() { return prevEl(this); }
	get innerHTML() { return this.childNodes.map(html).join(''); }
	set innerHTML(v) { while (this.firstChild) this.removeChild(this.firstChild); for (const h of N.parse(String(v))) N.append(this._h, h); }
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
	getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
	getClientRects() { return []; }
	get offsetWidth() { return 0; } get offsetHeight() { return 0; } get clientWidth() { return 0; } get clientHeight() { return 0; }
	get offsetTop() { return 0; } get offsetLeft() { return 0; } get offsetParent() { return this.parentElement; }
	get scrollTop() { return 0; } set scrollTop(v) {} get scrollLeft() { return 0; } set scrollLeft(v) {}
	get scrollHeight() { return 0; } get scrollWidth() { return 0; }
	attachShadow() { return this; }
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
	get naturalWidth() { return +this.getAttribute('width') || 0; } get naturalHeight() { return +this.getAttribute('height') || 0; }
	get complete() { return true; }
	get dir() { return this.getAttribute('dir') || ''; } get lang() { return this.getAttribute('lang') || ''; }
	submit() { navigateForm(this); } reset() {}
	play() { return Promise.resolve(); } pause() {} load() {}
	getContext() { return null; } toDataURL() { return ''; }
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
	get cookie() { return G.__cookies.join('; '); }
	set cookie(v) { const kv = String(v).split(';')[0]; const k = kv.split('=')[0]; G.__cookies = G.__cookies.filter(c => c.split('=')[0] !== k).concat([kv]); }
	get referrer() { return ''; } get domain() { return location.hostname; } get visibilityState() { return 'visible'; } get hidden() { return false; }
	get characterSet() { return 'UTF-8'; } get compatMode() { return 'CSS1Compat'; } get activeElement() { return this.body; }
	get forms() { return qsa(this, 'form'); } get images() { return qsa(this, 'img'); } get links() { return qsa(this, 'a[href]'); } get scripts() { return qsa(this, 'script'); }
	createElement(t) { return W(N.create(String(t).toLowerCase())); }
	createElementNS(ns, t) { return this.createElement(t); }
	createTextNode(s) { return W(N.createText(String(s))); }
	createComment(s) { return W(N.createText('')); }
	createDocumentFragment() { return W(N.createFrag()); }
	createEvent() { return new Event(''); }
	createRange() { return { selectNodeContents() {}, setStart() {}, setEnd() {}, collapse() {}, createContextualFragment: h => { const f = document.createDocumentFragment(); for (const x of N.parse(h)) N.append(f._h, x); return f; } }; }
	createTreeWalker(root) { const a = []; walkEls(root, e => { a.push(e); }); let i = -1; return { nextNode() { i++; return a[i] || null; }, get currentNode() { return a[i] || root; } }; }
	getElementById(id) { let r = null; id = String(id); walkEls(this, e => { if (N.getA(e._h, 'id') === id) { r = e; return true; } }); return r; }
	getElementsByTagName(t) { t = String(t).toLowerCase(); return qsa(this, t === '*' ? '*' : t); }
	getElementsByClassName(c) { return qsa(this, String(c).trim().split(/\s+/).map(x => '.' + x).join('')); }
	getElementsByName(n) { return qsa(this, '[name="' + n + '"]'); }
	querySelector(s) { return qs(this, s); }
	querySelectorAll(s) { return qsa(this, s); }
	write(...s) { const cur = G.__script; const nodes = N.parse(s.join('')); if (cur && cur.parentNode) { for (const h of nodes) N.insert(cur.parentNode._h, h, N.next(cur._h)); } else { const b = this.body; if (b) for (const h of nodes) N.append(b._h, h); } }
	writeln(...s) { this.write(...s, '\n'); }
	open() {} close() {}
	hasFocus() { return true; }
	execCommand() { return false; }
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
const proto = EventTarget.prototype;
G.addEventListener = proto.addEventListener; G.removeEventListener = proto.removeEventListener; G.dispatchEvent = proto.dispatchEvent;
G.__cookies = []; G.__ready = false; G.__script = null;

const loc0 = N.url;
class URLx {
	constructor(u, base) {
		u = String(u);
		const m0 = /^([a-zA-Z][\w+.-]*:)/.exec(u);
		if (!m0 && base !== undefined) u = resolveUrl(String(base), u);
		const m = /^([a-zA-Z][\w+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(u);
		if (!m) throw new TypeError('URL no válida: ' + u);
		this.protocol = m[1].toLowerCase(); this.host = m[2] || ''; this.pathname = m[3] || '/'; this.search = m[4] || ''; this.hash = m[5] || '';
		const hp = this.host.split(':'); this.hostname = hp[0]; this.port = hp[1] || '';
		this.origin = this.protocol + '//' + this.host; this.username = ''; this.password = '';
	}
	get href() { return this.protocol + (this.host || this.protocol === 'file:' ? '//' + this.host : '') + this.pathname + this.search + this.hash; }
	set href(v) { Object.assign(this, new URLx(v)); }
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
		if (p === 'reload') return () => {};
		if (p === 'toString') return () => t.href;
		return t[p];
	},
});
document.location = G.location;

G.navigator = { userAgent: N.ua, language: 'es-ES', languages: ['es-ES', 'es', 'en'], platform: 'Nintendo 3DS', cookieEnabled: true, onLine: true,
	vendor: '', appName: 'Netscape', appVersion: '5.0', maxTouchPoints: 1, hardwareConcurrency: 1, webdriver: false,
	sendBeacon() { return true; }, serviceWorker: undefined, clipboard: { writeText: () => Promise.resolve() }, mediaDevices: undefined };
G.screen = { width: N.width, height: 600, availWidth: N.width, availHeight: 600, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };
G.innerWidth = N.width; G.innerHeight = 600; G.outerWidth = N.width; G.outerHeight = 600; G.devicePixelRatio = 1;
G.scrollX = 0; G.scrollY = 0; G.pageXOffset = 0; G.pageYOffset = 0;
G.scrollTo = G.scroll = G.scrollBy = () => {};
G.alert = m => N.log('alert: ' + m); G.confirm = () => true; G.prompt = () => null; G.print = () => {};
G.open = u => { if (u) N.navigate(resolveUrl(location.href, String(u))); return null; };
G.close = () => {}; G.focus = () => {}; G.blur = () => {}; G.stop = () => {};
G.getComputedStyle = el => { const s = el && el.style; return new Proxy({}, { get(t, p) { if (p === 'getPropertyValue') return k => (s ? s.getPropertyValue(k) : ''); if (p === 'display') return el && el.hidden ? 'none' : ((s && s.display) || 'block'); return (s && typeof p === 'string' && s[p]) || ''; } }); };
G.matchMedia = q => ({ matches: /max-width/.test(q) ? N.width <= (+(/(\d+)/.exec(q) || [0, 0])[1]) : /prefers-color-scheme:\s*light/.test(q) || /min-width/.test(q) && N.width >= (+(/(\d+)/.exec(q) || [0, 0])[1]),
	media: q, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} });
G.history = { length: 1, state: null, pushState(s, t, u) { this.state = s; }, replaceState(s, t, u) { this.state = s; }, back() {}, forward() {}, go() {} };
const store = () => { const m = new Map(); return { getItem: k => (m.has(String(k)) ? m.get(String(k)) : null), setItem: (k, v) => m.set(String(k), String(v)), removeItem: k => m.delete(String(k)), clear: () => m.clear(), key: i => [...m.keys()][i] ?? null, get length() { return m.size; } }; };
G.localStorage = store(); G.sessionStorage = store();
G.console = { log: (...a) => N.log('console: ' + a.map(String).join(' ')), info() {}, debug() {}, warn() {}, error: (...a) => N.log('console.error: ' + a.map(String).join(' ')),
	trace() {}, group() {}, groupEnd() {}, groupCollapsed() {}, table() {}, time() {}, timeEnd() {}, assert() {}, dir() {}, count() {} };
G.performance = { now: () => N.now(), timing: { navigationStart: 0 }, mark() {}, measure() {}, getEntriesByType: () => [], getEntriesByName: () => [] };
G.crypto = { getRandomValues: a => { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256); return a; }, randomUUID: () => 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { const r = Math.random() * 16 | 0; return (c === 'x' ? r : r & 3 | 8).toString(16); }) };
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
G.btoa = s => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2); o += B64[a >> 2] + B64[(a & 3) << 4 | (b >> 4 || 0)] + (isNaN(b) ? '=' : B64[(b & 15) << 2 | (c >> 6 || 0)]) + (isNaN(c) ? '=' : B64[c & 63]); } return o; };
G.atob = s => { s = String(s).replace(/[^A-Za-z0-9+/]/g, ''); let o = ''; for (let i = 0; i < s.length; i += 4) { const a = B64.indexOf(s[i]), b = B64.indexOf(s[i + 1]), c = B64.indexOf(s[i + 2]), d = B64.indexOf(s[i + 3]); o += String.fromCharCode(a << 2 | b >> 4); if (c >= 0 && s[i + 2]) o += String.fromCharCode((b & 15) << 4 | c >> 2); if (d >= 0 && s[i + 3]) o += String.fromCharCode((c & 3) << 6 | d); } return o; };
class Observer { constructor(cb) { this.cb = cb; } observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } }
G.MutationObserver = Observer; G.ResizeObserver = Observer; G.PerformanceObserver = Observer;
G.IntersectionObserver = class extends Observer { observe(el) { const cb = this.cb; setTimeout(() => { try { cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: el.getBoundingClientRect() }], this); } catch (e) { report(e); } }, 0); } };
G.requestIdleCallback = f => setTimeout(() => f({ timeRemaining: () => 50, didTimeout: false }), 1);
G.cancelIdleCallback = id => clearTimeout(id);
G.Image = class { constructor(w, h) { const e = document.createElement('img'); if (w) e.setAttribute('width', w); if (h) e.setAttribute('height', h); return e; } };
G.Option = class { constructor(t, v) { const e = document.createElement('option'); e.textContent = t || ''; if (v !== undefined) e.setAttribute('value', v); return e; } };
G.CSS = { supports: () => false, escape: s => String(s).replace(/[^\w-]/g, c => '\\' + c) };
G.DOMParser = class { parseFromString(s) { const f = document.createDocumentFragment(); for (const h of N.parse(String(s))) N.append(f._h, h); return { body: f, documentElement: f, querySelector: x => qs(f, x), querySelectorAll: x => qsa(f, x), getElementById: id => qs(f, '#' + id) }; } };
G.TextEncoder = class { encode(s) { const u = unescape(encodeURIComponent(String(s))); const a = new Uint8Array(u.length); for (let i = 0; i < u.length; i++) a[i] = u.charCodeAt(i); return a; } };
G.TextDecoder = class { decode(a) { if (!a) return ''; let s = ''; for (const b of new Uint8Array(a.buffer || a)) s += String.fromCharCode(b); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } } };
G.Blob = class { constructor(parts) { this._t = (parts || []).join(''); this.size = this._t.length; } text() { return Promise.resolve(this._t); } };
G.FormData = class { constructor(form) { this._p = []; if (form) for (const e of form.querySelectorAll('input[name],select[name],textarea[name]')) this._p.push([e.name, e.value]); } append(k, v) { this._p.push([k, String(v)]); } get(k) { const e = this._p.find(x => x[0] === k); return e ? e[1] : null; } entries() { return this._p[Symbol.iterator](); } };
G.AbortController = class { constructor() { this.signal = { aborted: false, addEventListener() {}, removeEventListener() {} }; } abort() { this.signal.aborted = true; } };
G.WebSocket = class { constructor() { setTimeout(() => this.onerror && this.onerror(new Event('error')), 0); } send() {} close() {} };
G.Worker = class { postMessage() {} terminate() {} addEventListener() {} };
G.BroadcastChannel = class { postMessage() {} close() {} addEventListener() {} };
G.customElements = { define() {}, get() {}, whenDefined: () => Promise.resolve() };
G.speechSynthesis = undefined;

/* ---------- red: fetch y XMLHttpRequest (las respuestas llegan sin esperar: va en su hilo) */
function doFetch(url, method, body) {
	const r = N.fetch(resolveUrl(location.href, String(url)), method || 'GET', body == null ? null : String(body));
	if (!r) throw new TypeError('Failed to fetch');
	return { status: r[0], text: r[1], url: r[2] };
}
class Response {
	constructor(r) { this._r = r; this.status = r.status; this.ok = r.status >= 200 && r.status < 300; this.url = r.url; this.statusText = ''; this.redirected = false; this.headers = { get: () => null, has: () => false, forEach() {} }; }
	text() { return Promise.resolve(this._r.text); }
	json() { try { return Promise.resolve(JSON.parse(this._r.text)); } catch (e) { return Promise.reject(e); } }
	blob() { return Promise.resolve(new Blob([this._r.text])); }
	arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._r.text).buffer); }
	clone() { return new Response(this._r); }
}
G.Response = Response; G.Headers = class { constructor() {} get() { return null; } set() {} append() {} has() { return false; } };
G.Request = class { constructor(u, o) { this.url = String(u); Object.assign(this, o || {}); } };
G.fetch = (u, o) => { try { o = o || {}; if (u && u.url) u = u.url; return Promise.resolve(new Response(doFetch(u, o.method, o.body))); } catch (e) { return Promise.reject(e); } };
G.XMLHttpRequest = class extends EventTarget {
	constructor() { super(); this.readyState = 0; this.status = 0; this.responseText = ''; this.response = ''; this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.upload = { addEventListener() {} }; }
	open(m, u) { this._m = m; this._u = u; this.readyState = 1; }
	setRequestHeader() {} getResponseHeader() { return null; } getAllResponseHeaders() { return ''; } overrideMimeType() {} abort() {}
	send(body) {
		setTimeout(() => {
			try { const r = doFetch(this._u, this._m, body); this.status = r.status; this.responseText = r.text; this.responseURL = r.url;
				this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(r.text); } catch (e) { return null; } })() : r.text; }
			catch (e) { this.status = 0; }
			this.readyState = 4;
			const fire = t => { const ev = new Event(t); ev.target = this; if (typeof this['on' + t] === 'function') try { this['on' + t](ev); } catch (e) { report(e); } };
			fire('readystatechange'); fire(this.status ? 'load' : 'error'); fire('loadend');
		}, 0);
	}
};

/* ---------- temporizadores: corren con un reloj propio, sin esperar de verdad */
let tid = 0, vnow = 0; const timers = new Map();
G.setTimeout = (f, ms, ...a) => { const id = ++tid; timers.set(id, { f, at: vnow + Math.max(0, +ms || 0), a, every: 0 }); return id; };
G.setInterval = (f, ms, ...a) => { const id = ++tid; const e = Math.max(10, +ms || 0); timers.set(id, { f, at: vnow + e, a, every: e }); return id; };
G.clearTimeout = G.clearInterval = id => timers.delete(id);
G.requestAnimationFrame = f => setTimeout(() => f(N.now()), 16);
G.cancelAnimationFrame = id => clearTimeout(id);
G.queueMicrotask = f => Promise.resolve().then(f);
G.setImmediate = f => setTimeout(f, 0);
G.__timers = limit => {
	let ran = 0;
	for (let guard = 0; guard < 2000; guard++) {
		let best = null, bid = 0;
		for (const [id, t] of timers) if (!best || t.at < best.at) { best = t; bid = id; }
		if (!best || best.at > limit) break;
		vnow = Math.max(vnow, best.at);
		if (best.every) best.at += best.every; else timers.delete(bid);
		try { typeof best.f === 'function' ? best.f(...best.a) : (0, eval)(String(best.f)); } catch (e) { report(e); }
		ran++;
		if (best.every && ran > 200) break;
	}
	return ran > 0 && [...timers.values()].some(t => t.at <= limit && !t.every);
};
G.__click = h => {
	const el = W(h); if (!el) return true;
	const ev = new G.MouseEvent('click', { bubbles: true, cancelable: true });
	ev.button = 0; ev.which = 1;
	el.dispatchEvent(ev);
	// <label for> y casillas: como un navegador
	if (!ev.defaultPrevented && el.localName === 'label') { const f = el.getAttribute('for'); const t = f ? document.getElementById(f) : el.querySelector('input'); if (t && t !== el) t.click(); }
	return !ev.defaultPrevented;
};
G.__current = s => { G.__script = s ? W(s._h || s) : null; };
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
})(globalThis);
