// Scientific keypad. Shares the expression and answer with the main page through
// localStorage; degrees/radians is remembered too.
const exprEl = document.getElementById('expr');
const valueEl = document.getElementById('value');
const modeEl = document.getElementById('mode');
let expr = localStorage.getItem('expr') || '';
let ans = +localStorage.getItem('ans') || 0;
let deg = localStorage.getItem('deg') !== 'false';

function show(result) {
  exprEl.textContent = result !== undefined ? expr + ' =' : expr;
  modeEl.textContent = deg ? 'DEG' : 'RAD';
  valueEl.classList.remove('error');
  if (result !== undefined) {
    valueEl.textContent = result;
    return;
  }
  let preview = '';
  try {
    if (expr) preview = Calc.format(Calc.evaluate(expr, {ans, deg}));
  } catch (_) {}
  valueEl.textContent = preview || '0';
  localStorage.setItem('expr', expr);
}

function press(k) {
  if (k === 'AC') expr = '';
  else if (k === 'back') expr = expr.replace(/(sin\(|cos\(|tan\(|ln\(|log\(|√\(|Ans|.)$/, '');
  else if (k === 'EXP') expr += 'E';
  else if (k === '=') {
    if (!expr) return;
    try {
      const v = Calc.evaluate(expr, {ans, deg});
      if (!isFinite(v)) throw new Error('not a number');
      const text = Calc.format(v);
      Calc.saveHistory(expr, text);
      ans = v;
      localStorage.setItem('ans', String(ans));
      show(text);
      expr = '';
      localStorage.setItem('expr', '');
    } catch (_) {
      valueEl.textContent = 'Error';
      valueEl.classList.add('error');
    }
    return;
  } else expr += k;
  show();
}

document.getElementById('keys').addEventListener('click', (e) => {
  const key = e.target.closest('.key');
  if (key) press(key.dataset.k);
});
modeEl.addEventListener('click', () => {
  deg = !deg;
  localStorage.setItem('deg', String(deg));
  show();
});
document.getElementById('back').addEventListener('click', () => navigate(-1));

show();
