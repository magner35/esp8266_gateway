function vBuild() {
  loadWidgets();
  $('values').innerHTML = widgets.filter(w => w.on).map(widgetHtml).join('');
  vBuilt = true; chartDrawAll(); setDrag(); renderPanel();
}
let editing = false;
function toggleEdit() {
  editing = !editing;
  document.body.className = editing ? 'edit' : '';
  $('btnEdit').classList.toggle('act', editing);
  setDrag();
}
function setDrag() {
  const el = $('values'); if (!el) return;
  [...el.children].forEach(c => c.draggable = editing);
}
let dragK = null;
document.addEventListener('dragstart', e => {
  const c = e.target.closest && e.target.closest('.vcard');
  if (!editing || !c) return;
  dragK = c.dataset.k; c.classList.add('dragging');
  e.dataTransfer.effectAllowed = 'move';
});
document.addEventListener('dragend', e => {
  const c = e.target.closest && e.target.closest('.vcard');
  if (c) c.classList.remove('dragging');
  dragK = null;
});
document.addEventListener('dragover', e => {
  if (!editing || !dragK) return;
  if (e.target.closest && e.target.closest('.vcard')) e.preventDefault();
});
/*
 * FLIP-анимация перестановки: перед изменением DOM запоминаем позиции
 * карточек, после - сдвигаем каждую на её СТАРОЕ место через transform
 * и анимируем возврат в ноль. Дёшево: одна пара rect + rAF на карточку.
 */
function flipReorder(mutate) {
  const pos = new Map();
  for (const c of $('values').children)
    pos.set(c, c.getBoundingClientRect());
  mutate();
  for (const c of $('values').children) {
    const b = pos.get(c);
    if (!b) continue;
    const r = c.getBoundingClientRect();
    const dx = b.left - r.left, dy = b.top - r.top;
    if (!dx && !dy) continue;
    c.style.transition = 'none';
    c.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
    requestAnimationFrame(() => {
      c.style.transition = 'transform .2s ease-out';
      c.style.transform = '';
    });
  }
}
/* DOM-порядок карточек = порядок виджетов; сохраняем его */
function saveOrderFromDom() {
  const ord = [...$('values').children].map(x => x.dataset.k);
  widgets.sort((a, b) =>
    (ord.indexOf(a.k) + 1 || 1e9) - (ord.indexOf(b.k) + 1 || 1e9));
  saveWidgets();
}
document.addEventListener('drop', e => {
  const c = e.target.closest && e.target.closest('.vcard');
  if (!editing || !dragK || !c || c.dataset.k === dragK) return;
  e.preventDefault();
  const src = document.querySelector('.vcard[data-k="' + dragK + '"]');
  if (!src) return;
  /* insert before/after by the pointer half of the target card */
  const r = c.getBoundingClientRect();
  const after = (e.clientY - r.top) > r.height / 2 || (e.clientX - r.left) > r.width / 2;
  flipReorder(() => c.parentNode.insertBefore(src, after ? c.nextSibling : c));
  saveOrderFromDom();
});
/*
 * МОБИЛЬНЫЙ ВАРИАНТ: HTML5 drag-n-drop на тач-экранах не работает
 * (dragstart/drop - события мыши). В режиме правки таскаем карточку
 * пальцем: touchmove ищет карточку под пальцем и переставляет живьём,
 * touchend фиксирует порядок. Скролл блокируем только после того,
 * как таскание началось, чтобы страница оставалась прокручиваемой.
 */
let touchDrag = null;
document.addEventListener('touchstart', e => {
  if (!editing) return;
  const c = e.target.closest && e.target.closest('.vcard');
  if (!c) return;
  touchDrag = { el: c, moved: false };
}, { passive: true });
document.addEventListener('touchmove', e => {
  if (!editing || !touchDrag) return;
  const t = e.touches[0];
  const over = document.elementFromPoint(t.clientX, t.clientY);
  const tgt = over && over.closest && over.closest('.vcard');
  if (tgt && tgt !== touchDrag.el) {
    const r = tgt.getBoundingClientRect();
    const after = (t.clientY - r.top) > r.height / 2 || (t.clientX - r.left) > r.width / 2;
    flipReorder(() => tgt.parentNode.insertBefore(touchDrag.el, after ? tgt.nextSibling : tgt));
    touchDrag.moved = true;
  }
  if (touchDrag.moved) e.preventDefault();   /* тянем - скролл не нужен */
}, { passive: false });
document.addEventListener('touchend', () => {
  if (editing && touchDrag && touchDrag.moved) saveOrderFromDom();
  touchDrag = null;
});
/* vApply: раз в кадр 'm' обновляет карточки всех типов виджетов */
function vApply(d) {
  const barLo = vApply.lo || (vApply.lo = {}), barHi = vApply.hi || (vApply.hi = {});
  for (const n of VFLOATS) { const e = $('vf_' + n); if (e && d[n] !== undefined) e.textContent = d[n] }
  for (const n of VUINTS) { const e = $('vf_' + n); if (e && d[n] !== undefined) e.textContent = d[n] }
  /* 'cval' КАСТОМНЫЕ: формула calc(d, MY_CONST) из блока CUSTOM */
  for (const w of widgets || []) {
    if (w.t !== 'cval' || !w.on) continue;
    const c = CUSTOM[w.k], e = $('vfc_' + w.k);
    if (c && e && c.calc) {
      /* no forced formatting here: the calc's own rounding (e.g.
       * Number(x.toPrecision(4))) is what the card must show */
      const v = c.calc(d, MY_CONST);
      if (isFinite(v)) e.textContent = String(v)
    }
  }
  /* 'par' ПАРАМЕТР: авто = как отдал прибор, иначе пересчёт в
   * выбранные единицы (объём, для расхода ещё и период).
   * Точность вывода: float - 8 знаков включая точку,
   * uint32 (Импульсы) - до 10 знаков */
  function fmtF8(v) {
    for (let dec = 7; dec >= 0; dec--) {
      const s = v.toFixed(dec);
      if (s.length <= 8) return s;
    }
    return v.toFixed(0).slice(0, 8);
  }
  function fmtU10(v) {
    return String(Math.round(v)).slice(0, 10);
  }
  for (const w of widgets || []) {
    if (w.t !== 'par' || !w.on) continue;
    const e = $('vp_' + w.k), v = parValue(w, d);
    if (e && v !== undefined && isFinite(v))
      e.textContent = CHSRC[w.src].un === 'none' ? fmtU10(v) : fmtF8(v);
    const u = $('vu_' + w.k);
    if (u) u.textContent = parUnit(w);
  }
  /* 'inp' уставки дозатора: подсказка единиц - всегда глобальные */
  for (const w of widgets || []) {
    if (w.t !== 'inp' || !w.on) continue;
    const u = $('iu_' + w.k);
    if (u) u.textContent = UN.obj || '';
  }
  /* 'bit' БИТ: лампа зажигается выбранным цветом при бите = 1 */
  for (const w of widgets || []) {
    if (w.t !== 'bit' || !w.on) continue;
    const e = $('bl_' + w.k);
    if (e && d[w.src] !== undefined) {
      const on = (d[w.src] >> w.b) & 1;
      e.style.background = on ? w.col : BITOFF;
      e.style.color = w.col;               /* currentColor glow */
      e.classList.toggle('on', !!on);
    }
  }
  /* 'tot' ОБЪЁМ мигрировал в 'par' — отдельной обработки нет */
  /* 'bar' БАРГРАФ: заполнение = значение / уставка режима (100%) */
  for (const w of widgets || []) {
    if (w.t !== 'bar' || !w.on) continue;
    const m = BARM[w.mode], f = $('hf_' + w.k);
    let v = m ? d[m.src] : undefined;
    /* ИСКЛЮЧЕНИЕ "Расход (Qmax)": всегда считается в л/мин - уставка
     * Qmax задана в л/мин, а кадр 'm' приходит в глобальных единицах.
     * X за ds секунд -> за минуту: X * 60/ds (множитель 60/ds!) */
    if (w.mode === 'qmax' && v !== undefined) {
      const devKey = UNKEY[UN.rate] || 'l';
      const ds = PSEC[{ 'с': 's', 'мин': 'm', 'ч': 'h' }[UN.time]] || 60;
      v = v * (VOLU[devKey] / VOLU.l) * (60 / ds);
    }
    if (f && v !== undefined) {
      /* 100% = the setpoint parameter of the mode (paramsTick);
       * fallback until it is known: running min/max like the chart */
      let hi = SP[w.mode];
      if (!(hi > 0)) {
        if (!(w.k in barLo) || v < barLo[w.k]) barLo[w.k] = v;
        if (!(w.k in barHi) || v > barHi[w.k]) barHi[w.k] = v;
        hi = barHi[w.k];
        if (hi - (barLo[w.k] || 0) < 1e-9) hi = (barLo[w.k] || 0) + 1;
      }
      const pct = Math.max(0, Math.min(100, v / hi * 100));
      f.style.width = pct.toFixed(1) + '%';
      const t = $('bp_' + w.k);
      if (t) t.textContent = pct.toFixed(0) + '%';
      /* уставка-100% с единицами, прижата вправо в строке имени;
       * для Qmax единицы фиксированы - л/мин (уставка в них и задана) */
      const su = $('su_' + w.k);
      if (su) {
        if (SP[w.mode] > 0) {
          const u = UN[m.un];
          const suffix = w.mode === 'qmax' ? 'л/мин'
            : m.un === 'rate' ? u + (UN.time ? '/' + UN.time : '') : u;
          su.textContent = Number(SP[w.mode].toPrecision(4)) + (suffix ? ' ' + suffix : '');
        } else su.textContent = '';
      }
    }
  }
}
async function valuesTick() {
  try {
    const d = await (await fetch('/api/values')).json();
    if (d.link !== undefined) {
      $('link').textContent = d.link ? 'ONLINE' : 'OFFLINE';
      $('link').className = 'tag ' + (d.link ? 'ok' : 'bad');
    }
    if (!d.ok) { return }
    if (!vBuilt) vBuild();
    vApply(d);
    /* every enabled chart widget appends its own source value */
    for (const w of widgets || [])
      if (w.t === 'chart' && w.on && CHSRC[w.src]) {
        const v = d[CHSRC[w.src].src];
        if (v !== undefined) { chartPush(w.k, v, CHPER[w.per] ? w.per : 600); chartDraw(w) }
      }
  } catch (e) { }
}
setInterval(valuesTick, 200); valuesTick();
/* отладочная информация (fw/ip/rssi/uptime/heap) переехала на /panel */
paramsTick(); setInterval(paramsTick, 30000);
wSync();
/* /panel открывает режимы правки ссылками с query-параметрами */
if (location.search.includes('edit')) toggleEdit();

