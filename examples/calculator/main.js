// Standard calculator. The expression being typed and the last answer live in localStorage
// (the app's memory.json), so the other pages and the next launch see them.
const exprEl = document.getElementById('expr');
const valueEl = document.getElementById('value');
let expr = localStorage.getItem('expr') || '';
let ans = +localStorage.getItem('ans') || 0;
let justEvaluated = false;

const isOp = (c) => '+−×÷'.includes(c);

function show() {
  exprEl.textContent = expr;
  valueEl.classList.remove('error');
  let preview = '';
  if (expr) {
    try {
      preview = Calc.format(Calc.evaluate(expr, {ans}));
    } catch (_) {
      preview = '';
    }
  }
  valueEl.textContent = justEvaluated ? Calc.format(ans) : preview || expr.split(/[+−×÷]/).pop() || '0';
  localStorage.setItem('expr', expr);
}

function press(k) {
  if (k === 'AC') {
    expr = '';
    justEvaluated = false;
  } else if (k === 'back') {
    expr = expr.slice(0, -1);
    justEvaluated = false;
  } else if (k === '=') {
    if (!expr) return;
    try {
      const v = Calc.evaluate(expr, {ans});
      if (!isFinite(v)) throw new Error('not a number');
      const text = Calc.format(v);
      Calc.saveHistory(expr, text);
      ans = v;
      localStorage.setItem('ans', String(ans));
      exprEl.textContent = expr + ' =';
      expr = '';
      justEvaluated = true;
      valueEl.classList.remove('error');
      valueEl.textContent = text;
      localStorage.setItem('expr', '');
    } catch (_) {
      valueEl.textContent = 'Error';
      valueEl.classList.add('error');
    }
    return;
  } else if (k === '±') {
    // negate the last number
    const m = expr.match(/(.*?)(−?)([\d.]+)$/);
    if (m) expr = m[1] + (m[2] ? '' : '−') + m[3];
    else if (!expr) expr = '−';
  } else {
    if (justEvaluated) {
      // an operator continues from the answer, a digit starts over
      expr = isOp(k) || k === '%' ? 'Ans' + k : '';
      justEvaluated = false;
      if (isOp(k) || k === '%') return show();
    }
    if (isOp(k) && isOp(expr.slice(-1))) expr = expr.slice(0, -1);  // replace the operator
    if (k === '.') {
      const last = expr.split(/[+−×÷()]/).pop();
      if (last.includes('.')) return;
      if (!last) expr += '0';
    }
    expr += k;
  }
  show();
}

document.getElementById('keys').addEventListener('click', (e) => {
  const key = e.target.closest('.key');
  if (key) press(key.dataset.k);
});
document.getElementById('toScientific').addEventListener('click', () => navigate('scientific/main.html'));
document.getElementById('toHistory').addEventListener('click', () => navigate('history/main.html'));

// App-scope background process: keeps data/stats.json in step with the number of
// calculations, also while the scientific or history page is open.
setBackgroundProcess('stats', 'APP', () => {
  const count = +localStorage.getItem('count') || 0;
  let saved = -1;
  try {
    saved = fs.readJSON('stats.json').count;
  } catch (_) {}
  if (saved !== count) fs.writeJSON('stats.json', {count, updated: Date.now()});
}, 2000);

show();
