// Shared by the calculator's pages: expression evaluation (no eval), number formatting and the
// history file (data/history.txt, one "expression = result <TAB> time" per line).
const Calc = (() => {
  const FUNCS = {
    sin: (x, d) => Math.sin(d ? (x * Math.PI) / 180 : x),
    cos: (x, d) => Math.cos(d ? (x * Math.PI) / 180 : x),
    tan: (x, d) => Math.tan(d ? (x * Math.PI) / 180 : x),
    asin: (x, d) => (d ? (Math.asin(x) * 180) / Math.PI : Math.asin(x)),
    acos: (x, d) => (d ? (Math.acos(x) * 180) / Math.PI : Math.acos(x)),
    atan: (x, d) => (d ? (Math.atan(x) * 180) / Math.PI : Math.atan(x)),
    ln: (x) => Math.log(x),
    log: (x) => Math.log10(x),
    sqrt: (x) => Math.sqrt(x),
  };
  const factorial = (n) => {
    if (n < 0 || n !== Math.floor(n) || n > 170) return NaN;
    let r = 1;
    for (let i = 2; i <= n; i++) r *= i;
    return r;
  };

  function tokenize(src) {
    const out = [];
    let i = 0;
    while (i < src.length) {
      const c = src[i];
      if (c === ' ') { i++; continue; }
      if (/[0-9.]/.test(c)) {
        let j = i;
        while (j < src.length && /[0-9.]/.test(src[j])) j++;
        if (src[j] === 'E' && /[-+0-9]/.test(src[j + 1] || '')) {  // 1.5E-7
          j += 2;
          while (j < src.length && /[0-9]/.test(src[j])) j++;
        }
        out.push({t: 'num', v: parseFloat(src.slice(i, j))});
        i = j;
        continue;
      }
      if (/[a-z]/i.test(c)) {
        let j = i;
        while (j < src.length && /[a-z]/i.test(src[j])) j++;
        const w = src.slice(i, j).toLowerCase();
        if (w === 'ans') out.push({t: 'ans'});
        else if (w === 'e') out.push({t: 'num', v: Math.E});
        else if (FUNCS[w]) out.push({t: 'fn', v: w});
        else throw new Error('unknown ' + w);
        i = j;
        continue;
      }
      const map = {'π': ['num', Math.PI], '√': ['fn', 'sqrt'], '×': ['op', '*'], '*': ['op', '*'], '÷': ['op', '/'],
        '/': ['op', '/'], '+': ['op', '+'], '−': ['op', '-'], '-': ['op', '-'], '^': ['op', '^'], '(': ['(', 0],
        ')': [')', 0], '!': ['post', '!'], '%': ['post', '%'], '²': ['post', '²']};
      const m = map[c];
      if (!m) throw new Error('unexpected ' + c);
      out.push({t: m[0], v: m[1]});
      i++;
    }
    return out;
  }

  function evaluate(src, opts) {
    opts = opts || {};
    const toks = tokenize(src);
    let p = 0;
    const peek = () => toks[p];
    const startsOperand = (t) => t && (t.t === 'num' || t.t === 'ans' || t.t === 'fn' || t.t === '(');
    function expr() {
      let v = term();
      while (peek() && peek().t === 'op' && (peek().v === '+' || peek().v === '-')) {
        const o = toks[p++].v;
        const r = term();
        v = o === '+' ? v + r : v - r;
      }
      return v;
    }
    function term() {
      let v = power();
      for (;;) {
        const t = peek();
        if (t && t.t === 'op' && (t.v === '*' || t.v === '/')) {
          p++;
          const r = power();
          v = t.v === '*' ? v * r : v / r;
        } else if (startsOperand(t)) {
          v *= power();  // implicit multiplication: 2π, 3(4+1)
        } else {
          return v;
        }
      }
    }
    function power() {
      const b = unary();
      if (peek() && peek().t === 'op' && peek().v === '^') {
        p++;
        return Math.pow(b, power());  // right associative
      }
      return b;
    }
    function unary() {
      const t = peek();
      if (t && t.t === 'op' && (t.v === '-' || t.v === '+')) {
        p++;
        const v = unary();
        return t.v === '-' ? -v : v;
      }
      return postfix();
    }
    function postfix() {
      let v = primary();
      while (peek() && peek().t === 'post') {
        const o = toks[p++].v;
        v = o === '!' ? factorial(v) : o === '%' ? v / 100 : v * v;
      }
      return v;
    }
    function primary() {
      const t = toks[p++];
      if (!t) throw new Error('incomplete');
      if (t.t === 'num') return t.v;
      if (t.t === 'ans') return opts.ans || 0;
      if (t.t === '(') {
        const v = expr();
        if (peek() && peek().t === ')') p++;  // a missing ")" at the end is forgiven
        return v;
      }
      if (t.t === 'fn') return FUNCS[t.v](unary(), !!opts.deg);
      throw new Error('unexpected ' + (t.v || t.t));
    }
    const v = expr();
    if (p < toks.length) throw new Error('unexpected ' + (toks[p].v || toks[p].t));
    return v;
  }

  function format(v) {
    if (!isFinite(v)) return 'Error';
    if (v === 0) return '0';
    const a = Math.abs(v);
    if (a >= 1e12 || a < 1e-9) return v.toExponential(6).replace(/\.?0+e/, 'E').replace('E+', 'E');
    return String(parseFloat(v.toPrecision(12)));
  }

  function saveHistory(expression, result) {
    const f = open('history.txt', 'a');
    f.writeLine(`${expression} = ${result}\t${Date.now()}`);
    f.close();
    localStorage.setItem('count', String((+localStorage.getItem('count') || 0) + 1));
  }

  function readHistory() {
    if (!fs.exists('history.txt')) return [];
    const f = open('history.txt', 'r');
    const lines = f.readLines();
    f.close();
    return lines.filter(Boolean).map((l) => {
      const [text, time] = l.split('\t');
      return {text, time: +time || 0};
    });
  }

  return {evaluate, format, saveHistory, readHistory};
})();
