function vBuild() {
  loadWidgets();
  $('values').innerHTML = widgets.filter(w => w.on).map(widgetHtml).join('');
  vBuilt = true; chartDrawAll(); setDrag(); renderPanel();
}
let editing = false;
function toggleEdit() {
  editing = !editing;
  const sec = document.getElementById('sec-main');
  /* classList, НЕ className: класс .sec нужен роутеру и CSS секций */
  if (sec) sec.classList.toggle('edit', editing);
  $('btnEdit').classList.toggle('act', editing);
  const g = $('gear');
  if (g) {
    /* обычный режим - якорь; режим правки - кнопка "Закрепить" */
    g.textContent = editing ? '\u2693' : '\u2699';
    g.classList.toggle('ok', editing);
    g.title = editing ? 'Закрепить раскладку' : 'Панель';
  }
  setDrag();
}
/* якорь: обычный режим - на панель; в правке - "Закрепить":
 * сохранить компоновку виджетов и выйти из режима правки */
function gearClick() {
  if (editing) { wSave(); toggleEdit() }
  else routeTo('panel');
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
/*
 * ЕДИНСТВЕННЫЙ периодический запрос главной страницы: /api/values
 * везёт и значения, и link/ready, и ap/ip/ssid (баннер портала).
 * Отдельного /api/info-опроса здесь больше нет — экономим сокеты
 * ESP8266 (их всего 7, httpd однопоточный).
 *
 * Темп адаптивный (медленный/зашумлённый канал):
 *   - следующий запрос строго ПОСЛЕ завершения предыдущего;
 *   - интервал >= max(1с, 3 x RTT) — на медленном канале темп
 *     сам растягивается, очередь запросов не образуется;
 *   - таймаут 5с (AbortController) — зависший запрос не блокирует
 *     цикл навсегда;
 *   - при ошибках экспоненциальный откат x2 до 10с, после двух
 *     успехов — возврат к базовому темпу.
 */
let portalMode = false;
let pollRtt = 0, pollFails = 0;
/* КОНТРОЛЬНАЯ ТОЧКА "адаптивный одиночный опрос" (ESP8266, мало
 * сокетов, медленный/зашумлённый канал): единственный периодический
 * запрос /api/values — везёт значения, link/ready и ap/ip/ssid для
 * баннера портала. Следующий запрос строго ПОСЛЕ предыдущего;
 * таймаут 5с (AbortController); интервал >= max(1с, 3 x RTT) — на
 * медленном канале темп сам растягивается; при ошибках откат x2
 * до 10с. */
const POLL_BASE = 1000, POLL_MAX = 10000;

function portalBanner(ap, ssid, ip) {
  let b = document.getElementById('apbanner');
  if (ap) {
    if (!b) {
      b = document.createElement('a');
      b.id = 'apbanner';
      b.href = '/wifi';
      document.querySelector('header').after(b);
    }
    b.textContent = 'WiFi не подключён — шлюз в режиме точки доступа ' +
      (ssid || '') + ' (открытая), адрес http://' +
      (ip || '192.168.4.1') + '. Нажмите, чтобы выбрать сеть →';
  } else if (b) b.remove();
}

let lastSeqT = 0;      /* серверный ts последнего проигранного снимка */
let lastCfg = null;    /* версия настроек прибора из кадра 'm' */
let replayTimers = [];

/* пачка снимков за секунду опроса СК-Э -> проиграть равномерно
 * по 200 мс: транспорт 1 запрос/с, отображение 5 Гц */
function replaySeq(seq, off) {
  for (const t of replayTimers) clearTimeout(t);
  replayTimers = [];
  if (!seq || !seq.length) return;
  const step = 1000 / seq.length;
  seq.forEach((sv, i) => {
    replayTimers.push(setTimeout(() => {
      if (!vBuilt) vBuild();
      vApply(sv);
      lastSeqT = sv.t;
      /* график питается теми же снимками: 5 точек/с без запросов.
       * Значение — через parValue (как у виджетов-параметров): он
       * умеет к-фактор/цену импульса из множителя прибора и прочие
       * производные; авто=true = всегда в единицах прибора */
      if (CHARTS_ON && widgets)
        for (const w of widgets)
          if (w.t === 'chart' && w.on && CHSRC[w.src]) {
            const val = parValue({ ...w, auto: true }, sv);
            if (val !== undefined && isFinite(val))
              chartAppend(w.src, val,
                          sv.t ? sv.t + off : undefined);
          }
    }, i * step));
  });
}

async function pollLoop() {
  const t0 = Date.now();
  let ok = false;
  try {
    const ctl = new AbortController();
    const to = setTimeout(() => ctl.abort(), 5000);
    const d = await (await fetch('/api/values?since=' + lastSeqT,
                                  { signal: ctl.signal })).json();
    clearTimeout(to);
    ok = !!d.ok;
    sLastResp = 'ok:' + (d.ok ? 1 : 0) +
      ' seq:' + (d.seq ? d.seq.length : 'нет') +
      (d.seq && d.seq.length
        ? ' rate:' + (d.seq[d.seq.length - 1].rate !== undefined
            ? d.seq[d.seq.length - 1].rate : 'НЕТ')
        : '');
    if (d.link !== undefined) {
      $('link').textContent = d.link ? 'ONLINE' : 'OFFLINE';
      $('link').className = 'tag ' + (d.link ? 'ok' : 'bad');
    }
    portalMode = !!d.ap;
    portalBanner(d.ap, d.ssid, d.ip);
    /* смена версии настроек прибора (меняли через меню/консоль СК-Э):
     * шлюз сам перечитывает дерево, мы - сразу подтягиваем параметры */
    if (d.cfg !== undefined && d.cfg !== lastCfg) {
      if (lastCfg !== null) paramsTick();
      lastCfg = d.cfg;
    }
    if (d.ok) replaySeq(d.seq, Date.now() - (d.now || Date.now()));
  } catch (e) { }
  pollRtt = Date.now() - t0;
  pollFails = ok ? 0 : pollFails + 1;
  let dly = Math.max(POLL_BASE, pollRtt * 3);
  if (pollFails > 0)
    dly = Math.min(POLL_MAX, POLL_BASE * Math.pow(2, pollFails));
  setTimeout(pollLoop, dly);
}
pollLoop();
/* отладочная информация (fw/ip/rssi/uptime/heap) переехала на /panel */
/* тяжёлый /api/params (~12КБ) НЕ грузим вместе со страницей (15КБ
 * gzip): два больших ответа одновременно съедали кучу ESP до ~8КБ и
 * страница грузилась нестабильно. Разносим: сначала страница, через
 * 3с параметры; дальше — раз в минуту (меняются редко, плюс они
 * обновляются по действиям set/run) */
setTimeout(paramsTick, 3000); setInterval(paramsTick, 60000);
wSync();

