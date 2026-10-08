    const $ = i => document.getElementById(i);
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    /* live values window: MeterData_t fields, names as the struct members;
     * bitwise members render as FSETPOINT_/FSTATUS_/FISR_ flag chips */
    const FLAGS = {
      setpoint: ['RATEQMAX', 'RATEHIGH', 'RATELOW', 'FLOW', 'TOTAL', 'GTOTAL', 'BATCH', 'PREEMPTION'],
      status: ['ACCESERROR', 'BATCHER_TIMEOUT', 'BATCHER_PAUSE', 'BATCHER_FINISH', 'RTCERROR', 'RAMERROR', 'REVERSE', 'LOGFULL'],
      isr: ['DOUT0', 'DOUT1', 'DOUT2', 'BUZZER', 'HIGHFREQ', 'LOWFREQ', 'DUTY', 'PHASE']
    };
    /* display labels for the flag chips (bit order must match FLAGS) */
    const FLBL = {
      RATEQMAX: 'Расх>Qмакс', RATEHIGH: 'Расх>макс', RATELOW: 'Расх<мин',
      FLOW: 'Поток', TOTAL: 'Объём', GTOTAL: 'Общий объём', BATCH: 'Порция',
      PREEMPTION: 'Прерывание',
      ACCESERROR: 'Ошибка доступа', BATCHER_TIMEOUT: 'Таймаут порции',
      BATCHER_PAUSE: 'Порция: пауза', BATCHER_FINISH: 'Порция: конец',
      RTCERROR: 'Ошибка RTC', RAMERROR: 'Ошибка RAM', REVERSE: 'Реверс',
      LOGFULL: 'Журнал полон',
      DOUT0: 'Вых0', DOUT1: 'Вых1', DOUT2: 'Вых2', BUZZER: 'Звук',
      HIGHFREQ: 'Частота>', LOWFREQ: 'Частота<', DUTY: 'Скважность', PHASE: 'Фаза'
    };
    // const VFLOATS = ['frequency', 'rate_raw', 'rate_fast', 'rateMLPM', 'rate', 'total_plus', 'total_minus', 'total', 'total_sum', 'totalml_plus', 'totalml_minus', 'gtotal', 'gtotalml', 'kf_value', 'batch'];
    const VFLOATS = ['kf_value', 'batch'];
    const VUINTS = ['pulses_packet', 'pulses'];
    /* display labels only: the JSON keys stay the MeterData_t member
     * names, so the 'm' data itself is untouched */
    const LBL = {
      frequency: 'Частота, Гц',
      rate_raw: 'Расход (raw)',
      rate_fast: 'Расход (fast)',
      rateMLPM: 'Расход, мл/мин',
      rate: 'Расход',
      total_plus: 'Объём +',
      total_minus: 'Объём −',
      total: 'Объём',
      total_sum: 'Объём (сумма)',
      totalml_plus: 'Объём +, мл',
      totalml_minus: 'Объём −, мл',
      gtotal: 'Общий объём',
      gtotalml: 'Общий объём, мл',
      kf_value: 'K-фактор',
      batch: 'Порция',
      pulses_packet: 'Импульсы (пакет)',
      pulses: 'Импульсы',
      setpoint: 'Уставки (биты)',
      status: 'Состояние (биты)',
      isr: 'Флаги ISR (биты)'
    };

    const prec7 = x => Number(x.toPrecision(7));
    const round2 = x => Number(x.toFixed(2));
    /*
     * ── CUSTOM VALUE WIDGETS ────────────────────────────────────────
     * A digital-parameter widget computed by a free-form formula:
     *   calc(d, C) - d = the whole live 'm' values object (fields are
     *   the names in VFLOATS/VUINTS above), C = the named constants
     *   from MY_CONST below. Returns a number.
     * To add your own widget by hand, add ONE line to CUSTOM, e.g.:
     *     total_x2: { calc: (d, C) => d.total * C.k, label: 'Объём ×k', on: true },
     * That's all: the widget auto-appears on the main screen and in
     * the "Виджеты" panel (a saved layout already in localStorage
     * picks the new entry up on the next page load).
     * 'on' is the initial state for a FIRST appearance only; later
     * on/off and drag order are the user's, kept in localStorage.
     */
    const MY_CONST = {
      my_const: 10
    };
    const CUSTOM = {
      // fast_plus_total: { calc: d => r7(d.rate_fast + 10) + d.total, label: 'Частота, Гц', on: true },
      frequency_hz: { calc: d => round2(d.frequency), label: 'Частота, Гц', on: true },
      k_factor: { calc: d => prec7(d.kf_value), label: 'К-фактор', on: true },
      imp_weight: { calc: d => prec7(d.kf_value), label: 'Цена имп', on: true },
    };
    /*
     * ── FLOW WIDGET ─────────────────────────────────────────────────
     * Configurable rate widget (like the bar, configured in the panel):
     * input is rateMLPM (ml/min), the two combos pick the volume unit
     * and the time unit; the label composes itself ("Расход, л/мин").
     * The "vol" widget below is the same for gtotal (volume only).
     */
    const VOLU = { ml: 1, l: 1000, m3: 1000000 };   /* ml per volume unit */
    const VOLN = { ml: 'мл', l: 'л', m3: 'м³' };
    const PSEC = { s: 1, m: 60, h: 3600 };          /* seconds per period */
    const PERN = { s: 'с', m: 'мин', h: 'ч' };
    /*
     * ── BAR WIDGET MODES ────────────────────────────────────────────
     * value source + the setpoint parameter that marks the 100% level
     * (resolved by name from /api/params, see paramsTick)
     */
    const BARM = {
      qmax: { n: 'Расход (Qmax)', src: 'rate', sp: 'Qmax,л', un: 'rate' },
      rate: { n: 'Расход', src: 'rate', sp: 'Расход+', un: 'rate' },
      total: { n: 'Объём', src: 'total', sp: 'Объём', un: 'obj' },
      gtotal: { n: 'Общий', src: 'gtotal', sp: 'Общий', un: 'ob' },
      dose: { n: 'Доза', src: 'batch', sp: 'Доза', un: 'obj' }
    };
    let SP = {};   /* mode -> setpoint 100% value, refreshed with the tree */
    /*
     * Measurement units from the device tree (Основные → Единицы
     * измерения): three enum params "Расход"/"Объём"/"Общий" with
     * Миллилитр|Литр|Метр куб. Values AND setpoints arrive already in
     * these units (the device converts both), so the bar only needs
     * the right label - no conversion on our side.
     */
    const UNSHORT = { 'Миллилитр': 'мл', 'Литр': 'л', 'Метр куб.': 'м³' };
    const PERSHORT = { 'Секунда': 'с', 'Минута': 'мин', 'Час': 'ч' };
    let UN = { rate: '', obj: '', ob: '', time: '' };
    /*
     * Multiplier data (feeds the 'par' kf/price widget sources):
     * Множитель: Тип (kf/price), Значение, Единицы (volume unit).
     * Updated by paramsTick from the device settings tree.
     */
    const MULM = { kf: 'К-фактор', price: 'Цена имп' };
    let MULT = { type: 'price', value: NaN };   /* from the params tree */
    let LINM = '';    /* Линеаризация → Метод (Выкл/Линейный/Сплайн-A/Б):
                       * ≠ Выкл => К-фактор динамический, из kf_value */
    let MULU = '';    /* объёмная единица множителя (меню Множитель → Единицы) */
    /*
     * ── PARAM WIDGET (t:'par') ─────────────────────────────────────
     * Replaces the old flow/vol/tot trio. One source combo (from CHSRC:
     * Расход, Объём/+/−/#, Общий, Доза), unit combos (period is enabled
     * for Расход only) and the "авто" checkbox: checked = show the value
     * as the device reports it (device units from UN); unchecked =
     * convert to the manually chosen units. Raw 'm' values come in the
     * DEVICE units, so conversion = volume ratio (+ period ratio for
     * rate: seconds of the device period / seconds of the target one).
     */
    /* device-unit short name -> our combo key (мл->ml, мин->m, ...) */
    const UNKEY = { 'мл': 'ml', 'л': 'l', 'м³': 'm3', 'с': 's', 'мин': 'm', 'ч': 'h' };
    /* while "авто" is on, keep the widget's vol/time mirroring the device
     * units - so unchecking "авто" continues FROM them, not from defaults */
    function parSyncAuto(w) {
      if (w.t !== 'par' || w.auto === false) return;
      const un = CHSRC[w.src].un;
      const u = un === 'kf' || un === 'price' ? MULU : UN[un];
      if (u && UNKEY[u]) w.vol = UNKEY[u];
      if (un === 'rate' && UNKEY[UN.time]) w.time = UNKEY[UN.time];
    }
    function parUnit(w) {
      const un = CHSRC[w.src].un;
      if (un === 'hz') return 'Гц';          /* частота - всегда Гц */
      if (un === 'none') return '';          /* импульсы - без единиц */
      if (un === 'kf' || un === 'price') {
        /* множитель: авто = единицы из меню Множитель -> Единицы */
        const u = w.auto !== false ? MULU : VOLN[w.vol];
        return un === 'kf' ? 'имп/' + u : u + '/имп';
      }
      const isRate = un === 'rate';
      if (w.auto !== false) {
        const u = UN[un];
        if (!u) return '';
        return isRate ? u + (UN.time ? '/' + UN.time : '') : u;
      }
      return isRate ? VOLN[w.vol] + '/' + PERN[w.time] : VOLN[w.vol];
    }
    function parValue(w, d) {
      const un = CHSRC[w.src].un;
      /* К-фактор / цена импульса. При включённой линеаризации
       * (Линеаризация → Метод ≠ Выкл) К-фактор ДИНАМИЧЕСКИЙ: прибор
       * интерполирует его по частоте сигнала, живое значение едет
       * в каждом кадре 'm' как kf_value. Цена = обратное число.
       * Без линеаризации — константа из Множителя (другой тип =
       * обратное). Вручную: K-фактор растёт с укрупнением единицы
       * (имп/мл -> имп/л = ×1000), цена наоборот падает (÷1000). */
      if (un === 'kf' || un === 'price') {
        let v;
        if (LINM && LINM !== 'Выкл') {
          const kv = d.kf_value;
          if (!(kv > 0)) return undefined;   /* потока нет / интерп. 0 */
          v = un === 'kf' ? kv : 1 / kv;
        } else {
          if (!isFinite(MULT.value) || MULT.value <= 0) return undefined;
          v = CHSRC[w.src].src === MULT.type ? MULT.value : 1 / MULT.value;
        }
        if (w.auto !== false) return v;
        const devK = VOLU[UNKEY[MULU]] || VOLU.l;   /* ml per device unit */
        const tgtK = VOLU[w.vol];                   /* ml per chosen unit */
        return un === 'kf' ? v * tgtK / devK : v * devK / tgtK;
      }
      const v = d[CHSRC[w.src].src];
      if (v === undefined) return undefined;
      if (w.auto !== false) return v;   /* already in device units */
      /* dev -> target: multiply by (ml per DEV unit)/(ml per TARGET) */
      /* глобальная единица прибора ('мл') -> ключ VOLU ('ml') через UNKEY */
      const devKey = UNKEY[UN[un]] || 'l';
      const k = VOLU[devKey] / VOLU[w.vol];
      if (un === 'rate') {
        /* X за ds секунд -> за ts секунд: X * ts/ds (не ds/ts!) */
        const ds = PSEC[{ 'с': 's', 'мин': 'm', 'ч': 'h' }[UN.time]] || 60;
        return v * k * (PSEC[w.time] / ds);
      }
      return v * k;
    }
    /*
     * ── BIT WIDGET ──────────────────────────────────────────────────
     * One flag bit as a lamp: src group (FLAGS above) + bit index,
     * col = the lamp color of the ACTIVE level. The label is the flag
     * name and wraps on spaces (narrow cards).
     */
    const BITCOL = ['#1d5c2f', '#a23b2e', '#2a5bdb', '#e3b341'];
    const BITOFF = '#14171c';  /* lamp color of the INACTIVE level */
    /*
     * ── BUTTON WIDGET (t:'btn') ─────────────────────────────────────
     * Square button the width of an indicator, label inside the body.
     * Four fixed kinds, each added by its own row (no combo). Actions
     * go through the settings tree: doser mode Стоп/Старт (param 089),
     * Сброс объёма (CMD 142); ПАУЗА has no dedicated parameter in the
     * current firmware and follows Стоп until one appears.
     */
    const BTN = {
      stop:  { n: 'СТОП',  col: '#a23b2e', h: 'стоп дозатора' },
      start: { n: 'СТАРТ', col: '#1d5c2f', h: 'старт дозатора' },
      pause: { n: 'ПАУЗА', col: '#b58a2e', h: 'пауза дозатора' },
      reset: { n: 'СБРОС', col: '#2a5bdb', h: 'сброс объёма' }
    };
    let BTNP = { mode: 89, reset: 142 };   /* param ids, refreshed by paramsTick */
    async function btnAct(mode) {
      try {
        if (mode === 'start') {
          await fetch('/api/set', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'id=' + BTNP.mode + '&v=' + encodeURIComponent('Старт') + '&pw='
          });
        } else if (mode === 'stop' || mode === 'pause') {
          await fetch('/api/set', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'id=' + BTNP.mode + '&v=' + encodeURIComponent('Стоп') + '&pw='
          });
        } else if (mode === 'reset') {
          await fetch('/api/run', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'id=' + BTNP.reset + '&pw='
          });
        }
      } catch (e) { }
    }
    /*
     * ── SETPOINT INPUT WIDGET (t:'inp') ─────────────────────────────
     * Doser setpoints from the Уставки section: Доза / Упреждение /
     * Перелив. Sized and padded like the bar card; the label carries a
     * units hint (the device volume unit). Edits go via /api/set.
     */
    const INPS = {
      dose: { n: 'Доза' },
      pre: { n: 'Упреждение' },
      over: { n: 'Перелив' }
    };
    let INP = {};   /* src -> {id, v} from the tree */
    async function wInpSet(k) {
      const w = widgets.find(x => x.k === k);
      const e = $('wi_' + k);
      if (!w || !e || !INP[w.src]) return;
      try {
        const body = 'id=' + INP[w.src].id + '&v=' + encodeURIComponent(e.value);
        const r = await (await fetch('/api/set', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body
        })).json();
        if (r.ok) { INP[w.src].v = parseFloat(r.value); paramsTick() }
        else alert((INPS[w.src].n) + ': ' + r.error + (r.wait ? ' (ждать ~' + r.wait + ' с)' : ''));
      } catch (e2) { alert('нет ответа от шлюза') }
    }
    /*
     * ── CHART WIDGET (t:'chart') ────────────────────────────────────
     * Configurable trend: src (what to plot), per (window, seconds),
     * col (line color from the fixed palette), auto (autoscale; off =
     * fixed 0..setpoint scale, the setpoint matching the source).
     * Long windows are decimated to ~1200 points so 24 h does not eat
     * memory: samples land every ceil(per/1200) seconds.
     */
    const CHSRC = {
      rate: { n: 'Расход', src: 'rate', sp: 'rate', un: 'rate' },
      total: { n: 'Объём', src: 'total', sp: 'total', un: 'obj' },
      tplus: { n: 'Объём +', src: 'total_plus', sp: 'total', un: 'obj' },
      tminus: { n: 'Объём −', src: 'total_minus', sp: 'total', un: 'obj' },
      tsum: { n: 'Объём #', src: 'total_sum', sp: 'total', un: 'obj' },
      gtotal: { n: 'Общий', src: 'gtotal', sp: 'gtotal', un: 'ob' },
      batch: { n: 'Доза', src: 'batch', sp: 'dose', un: 'obj' },
      /* множитель (данные из дерева Множитель, см. MULT/MULU) */
      kf: { n: 'К-фактор', src: 'kf', un: 'kf' },
      price: { n: 'Цена имп.', src: 'price', un: 'price' },
      freq: { n: 'Частота', src: 'frequency', un: 'hz' },  /* всегда Гц */
      pulses: { n: 'Импульсы', src: 'pulses', un: 'none' } /* без единиц */
    };
    /* единица измерения источника графика: расход = объём + период,
     * объёмы = единица объёма (как у барграфа, из UN); частота/множитель —
     * свои: К-фактор и цена в единицах множителя (Множитель → Единицы) */
    function chUnit(s) {
      const un = CHSRC[s].un;
      if (un === 'hz') return 'Гц';
      if (un === 'kf') return MULU ? 'имп/' + MULU : '';
      if (un === 'price') return MULU ? MULU + '/имп' : '';
      const u = UN[un];
      if (!u) return '';
      return un === 'rate' ? u + (UN.time ? '/' + UN.time : '') : u;
    }
    /* источники графика, для которых масштаб всегда авто
     * (уставок в приборе для них нет) */
    const CHART_AUTO_ONLY = ['kf', 'price', 'freq', 'pulses'];
    const CHPER = {
      60: '1 мин', 300: '5 мин', 600: '10 мин', 1800: '30 мин',
      3600: '60 мин', 86400: '24 ч'
    };
    /* классическая палитра + белый; комбобокс показывает только имена */
    const CHCOL = [
      { n: 'красный', c: '#e05252' },
      { n: 'оранжевый', c: '#e08a3c' },
      { n: 'жёлтый', c: '#e3d041' },
      { n: 'зелёный', c: '#3fbf5f' },
      { n: 'голубой', c: '#4cc7e0' },
      { n: 'синий', c: '#5a7de0' },
      { n: 'фиолетовый', c: '#9a5ae0' },
      { n: 'белый', c: '#e8e8e8' }
    ];
    /*
     * CHART WIDGET — отрисовка канвасом, без серверных буферов:
     * точки приходят из /api/values и копятся на странице
     * (chartLive), окно времени скользит, соединяются прямыми.
     */
    const chartLive = {};               /* src -> {vals[], ts[]} */
    /* Графики ВКЛЮЧЕНЫ: данные копятся из 5-Гц пачек /api/values
     * (chartAppend из replaySeq), серверных запросов нет вообще.
     * История после перезагрузки страницы не восстанавливается
     * (для этого нужен ринг на ESP, +2-4КБ). */
    const CHARTS_ON = true;
    const CHSCALE = {};                 /* w.k -> {lo, hi} стабильный автоскейл */
    /* "красивый" шаг сетки: 1/2/5·10^n */
    function niceStep(x) {
      if (!(x > 0) || !isFinite(x)) return 1;
      const p = Math.pow(10, Math.floor(Math.log10(x)));
      const d = x / p;
      return (d >= 5 ? 5 : d >= 2 ? 2 : 1) * p;
    }
    function chartCol(w) {
      const e = CHCOL.find(x => x.c === w.col);
      return e ? e.c : CHCOL[5].c;
    }
    function srcName(w) { return w.src; }
    /*
     * Live-буфер с прогрессивной децимацией:
     *   25 Гц, 3000 точек = 2 мин → декимация → 12.5 Гц, 4 мин
     *   → 6.25 Гц, 8 мин → 3.1 Гц, 16 мин
     * Буфер никогда не превышает 3000 точек, но время покрытия
     * удваивается на каждой децимации. График для окон ≤16 мин
     * целиком из live-данных — плавный, без серверных ступенек.
     */
    /*
     * Backfill из серверного буфера ESP: после загрузки страницы и
     * после дыр (обрыв связи) забираем недостающие точки с реальными
     * таймштампами (/api/chart?since=N). Часы сервера — от старта,
     * клиентские — эпоха: смещение берём из "now" в каждом ответе.
     * Точки строже последней клиентской пропускаются (монотонность).
     */
    async function chartBackfill(src) {
      try {
        let buf = chartLive[src];
        if (!buf) buf = chartLive[src] = { vals: [], ts: [], srv: 0 };
        const d = await (await fetch('/api/chart?since=' + (buf.srv || 0))).json();
        if (!d.t || !d.t.length) return;
        const off = Date.now() - d.now;
        for (let i = 0; i < d.t.length; i++) {
          const t = d.t[i] + off;
          if (buf.ts.length && t <= buf.ts[buf.ts.length - 1]) continue;
          buf.vals.push(d.v[i]);
          buf.ts.push(t);
          buf.srv = d.t[i];
        }
      } catch (e) { }
    }
    /*
     * Кормление графиков из valuesTick: при пустом буфере или дыре —
     * сначала backfill (дыра закроется реальными точками), затем
     * текущая live-точка. Дыра >60с — сброс и полная история заново.
     */
    async function chartFeed(d) {
      if (!CHARTS_ON) return;
      if (!widgets) return;
      const srcs = new Set();
      for (const w of widgets)
        if (w.t === 'chart' && w.on && CHSRC[w.src] &&
            d[CHSRC[w.src].src] !== undefined)
          srcs.add(w.src);
      for (const src of srcs) {
        const buf = chartLive[src];
        const gap = buf && buf.ts.length ?
            Date.now() - buf.ts[buf.ts.length - 1] : Infinity;
        if (gap > CHART_HARD_RESET_MS)
          chartLive[src] = { vals: [], ts: [], srv: 0 };
        if (!isFinite(gap) || gap > CHART_GAP_MS)
          await chartBackfill(src);
        chartAppend(src, d[CHSRC[src].src]);
      }
    }
    /* пороги устаревания данных */
    const CHART_STALE_MS = 2000;    /* вкладка была скрыта дольше — чистим */
    const CHART_HARD_RESET_MS = 60000; /* дыра в данных: свыше этого —
                                        * сброс, меньше — рисуем разрыв */
    const CHART_GAP_MS = 3000;      /* дыра больше этого = видимый разрыв */
    const CHART_EDGE_MS = 1500;     /* окно сдвинуто назад: точки рождаются
                                     * за правым краем и плавно въезжают;
                                     * > возраста свежего сэмпла (~1.0-1.4с) */
    /* диагностика накопления: счётчик и последняя причина отказа */
    let sAtt = 0, sFail = 'вызовов не было';
    let sLastResp = 'ответов не было';   /* пишет pollLoop */
    function chartAppend(src, value, srvT) {
      if (!CHARTS_ON) return;
      sAtt++;
      if (!isFinite(value)) { sFail = 'не число: ' + value; return; }
      let buf = chartLive[src];
      /* серверный ts сэмпла (если дали): ровный шаг 200мс без
       * джиттера таймеров проигрывания пачки */
      const t = srvT || Date.now();
      /*
       * Плохая связь: короткие/средние обрывы НЕ сбрасывают график —
       * потерянные точки всё равно неоткуда взять, а накопленное
       * честно. Дыра в данных рисуется видимым разрывом (chartDraw).
       * Сброс только на дыру >60с — прибор пропал всерьёз.
       * Свёрнутая вкладка чистится отдельно, по visibilitychange.
       */
      if (buf && buf.ts.length &&
          t - buf.ts[buf.ts.length - 1] > CHART_HARD_RESET_MS)
        buf = chartLive[src] = { vals: [], ts: [] };
      if (!buf) buf = chartLive[src] = { vals: [], ts: [] };
      /* строго монотонные timestamp: пропускаем дубль ( тот же мс) */
      if (buf.ts.length && t <= buf.ts[buf.ts.length - 1]) {
        sFail = 'не монотонно: ' + Math.round(t - buf.ts[buf.ts.length - 1]) + 'мс';
        return;
      }
      sFail = 'принято';
      buf.vals.push(value);
      buf.ts.push(t);
      if (buf.vals.length > 3000) {
        /* децимация: оставляем каждую вторую точку */
        const v2 = [], t2 = [];
        for (let i = 0; i < buf.vals.length; i += 2) {
          v2.push(buf.vals[i]);
          t2.push(buf.ts[i]);
        }
        buf.vals = v2;
        buf.ts = t2;
      }
    }
    /*
     * Разворачивание вкладки: если была скрыта дольше порога
     * устаревания — чистим буферы СРАЗУ, не дожидаясь первого
     * свежего ответа (иначе старая кривая висит ещё секунды).
     * Короткие переключения (<2с) график не сбрасывают.
     */
    let chartHiddenAt = 0;
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) { chartHiddenAt = Date.now(); return; }
      if (chartHiddenAt && Date.now() - chartHiddenAt > CHART_STALE_MS)
        for (const src in chartLive)
          chartLive[src] = { vals: [], ts: [] };
      chartHiddenAt = 0;
    });
    function chartDrawAll() {
      for (const w of widgets || []) if (w.t === 'chart' && w.on) chartDraw(w);
    }
    /* высота графика = 2 × высота карточки параметра */
    function chartHeight() {
      const par = document.querySelector('#values .vcard:not(.chart):not(.wbar):not(.wbit):not(.wbtn):not(.winp)');
      return par ? par.offsetHeight * 2 : 144;
    }
    function chartDraw(w) {
      if (!CHARTS_ON) {
        const cv0 = $('rc_' + w.k);
        if (cv0) {
          const g0 = cv0.getContext('2d');
          g0.clearRect(0, 0, cv0.width, cv0.height);
          g0.fillStyle = '#8b94a7'; g0.font = '12px system-ui';
          g0.fillText('графики отключены', 8, cv0.height / 2);
        }
        return;
      }
      const cv = $('rc_' + w.k); if (!cv) return;
      /* фаза накопления: ПОЛНЫЙ размер канваса (2x параметра),
       * надпись по центру — виджет всегда своей нормальной высоты */
      const chartEmpty = (msg) => {
        const dpr0 = window.devicePixelRatio || 1;
        const cw0 = cv.offsetWidth, ch0 = chartHeight();
        if (cv.width !== cw0 * dpr0 || cv.height !== ch0 * dpr0) {
          cv.width = cw0 * dpr0;
          cv.height = ch0 * dpr0;
        }
        const g0 = cv.getContext('2d');
        g0.setTransform(dpr0, 0, 0, dpr0, 0, 0);
        g0.clearRect(0, 0, cw0, ch0);
        g0.fillStyle = '#8b94a7'; g0.font = '12px system-ui';
        g0.fillText(msg, 8, ch0 / 2);
      };
      const buf = chartLive[srcName(w)];
      if (!buf || buf.vals.length < 2) {
        /* диагностика прямо в надписи: сколько точек реально в буфере */
        chartEmpty('накопление… (точек ' + (buf ? buf.vals.length : 0) +
                   ', вызовов ' + sAtt + ', ' + sFail +
                   ', ' + w.src + '→' +
                   (CHSRC[w.src] ? CHSRC[w.src].src : 'НЕТ') +
                   ' | ' + sLastResp + ')');
        return;
      }
      /* окно времени: последние w.per секунд */
      const per = CHPER[w.per] ? w.per : 600;
      const cutoff = Date.now() - CHART_EDGE_MS - per * 1000;
      /* ТОЛЬКО live-данные: без серверного merge (нет ступенек) */
      const pts = [];
      for (let i = 0; i < buf.vals.length; i++)
        if (buf.ts[i] >= cutoff)
          pts.push({ v: buf.vals[i], t: buf.ts[i] });
      if (pts.length < 2) {
        /* точки есть, но вне окна — таймштампы/окно расходятся */
        chartEmpty('нет точек в окне (буфер: ' + buf.vals.length +
                   ', в окне: ' + pts.length + ')');
        return;
      }
      /*
       * Медианный фильтр на 3 точки: одиночные выбросы (шум прибора)
       * срезает, монотонные участки не искажает (медиана монотонной
       * тройки = сама точка), запаздывания нет. По копии значений.
       */
      if (pts.length >= 3) {
        const srcv = [];
        for (const p of pts) srcv.push(p.v);
        for (let i = 1; i < pts.length - 1; i++) {
          const a = srcv[i - 1], b = srcv[i], c = srcv[i + 1];
          pts[i].v = a + b + c - Math.min(a, b, c) - Math.max(a, b, c);
        }
      }
      const col = chartCol(w);
      const dpr = window.devicePixelRatio || 1;
      const cw = cv.offsetWidth, ch = chartHeight();
      if (cv.width !== cw * dpr || cv.height !== ch * dpr) { cv.width = cw * dpr; cv.height = ch * dpr }
      const g = cv.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, cw, ch);
      g.strokeStyle = '#2a3140'; g.lineWidth = 1;
      for (let k = 1; k < 4; k++) { const y = ch * k / 4; g.beginPath(); g.moveTo(0, y); g.lineTo(cw, y); g.stroke() }
      let lo, hi, minV = Infinity, maxV = -Infinity;
      for (const p of pts) { if (p.v < minV) minV = p.v; if (p.v > maxV) maxV = p.v }
      if (w.auto === false && !CHART_AUTO_ONLY.includes(w.src)) {
        /*
         * Фиксированная шкала по уставкам прибора. График расхода:
         * min = Уставки→Расход-, max = Уставки→Расход+. Остальные
         * графики — своя уставка сверху (BARM sp), снизу 0.
         * Защита: если уставки не заданы/мусор (hi<=lo) — автоскейл.
         */
        if (CHSRC[w.src].un === 'rate') {
          hi = SP.rate;
          lo = SP['rate-'] || 0;
          if (!(hi > lo)) { hi = maxV || 1; lo = 0; }
        } else {
          hi = SP[CHSRC[w.src].sp];
          if (!(hi > 0)) hi = maxV || 1;
          lo = 0;
        }
      } else {
        /*
         * Стабилизация автоскейла: при 10 Гц пересчёт lo/hi из min/max
         * окна каждый кадр дрожит — вся кривая и заливка прыгают по
         * вертикали. Приводим границы к "красивой" сетке (шаг 1/2/5·10^n)
         * и держим прежнюю сетку, пока данные в неё помещаются; смена
         * масштаба — редкая и одним шагом (гистерезис на 30% диапазона).
         */
        let tlo = minV, thi = maxV;
        if (thi - tlo < 1e-9) { tlo -= 0.5; thi += 0.5 }
        const pad = (thi - tlo) * 0.08; tlo -= pad; thi += pad;
        const step = niceStep((thi - tlo) / 4);
        tlo = Math.floor(tlo / step) * step;
        thi = Math.ceil(thi / step) * step;
        const prev = CHSCALE[w.k];
        if (prev && minV >= prev.lo && maxV <= prev.hi &&
            (maxV - minV) > 0.3 * (prev.hi - prev.lo)) {
          lo = prev.lo; hi = prev.hi;
        } else {
          lo = tlo; hi = thi;
          CHSCALE[w.k] = { lo: lo, hi: hi };
        }
      }
      /*
       * X-ось — скользящее окно от "сейчас" назад: tMax = Date.now(),
       * tMin = tMax - per*1000. Окно движется непрерывно с RAF (60 Гц),
       * кривая плывёт плавно, без 200мс-скачков.
       */
      const tMax = Date.now() - CHART_EDGE_MS;
      const tMin = tMax - per * 1000;
      const tSpan = per * 1000;
      const X = t => cw * (t - tMin) / tSpan;
      const Y = v => ch - 2 - (v - lo) / (hi - lo) * (ch - 4);
      /*
       * Отрисовка без сплайнов: прямые отрезки между точками
       * (попиксельно, как interpolationLinear в C-либе). За последней
       * точкой — короткое продолжение по тренду (линейное, не дальше
       * ~1.5 интервалов), чтобы растущий край не был плоским хвостом.
       */
      const n = pts.length;
      /*
       * Мин/макс-конверт по колонкам пикселей (как осциллограф).
       * На окнах 5/10 мин при 10 Гц в пиксель попадает 3-7 сэмплов:
       * попиксельная выборка ломаной "кипит" — окно едет субпиксельно
       * каждый кадр, и в пиксель попадают разные сэмплы. Конверт
       * (мин и макс сэмплов колонки) от этого стабилен: вертикальный
       * размах колонки меняется только когда меняются сами данные.
       * В разреженных местах (1 сэмпл на колонку) конверт вырождается
       * в обычную линию. За последней точкой — трендовая экстраполяция.
       */
      const tPx = tSpan / cw;
      const top = [], bot = [];
      let idx = 0, lastDataT = 0;
      for (let px = 0; px <= cw; px++) {
        const tt0 = tMin + tPx * px, tt1 = tt0 + tPx;
        while (idx < n && pts[idx].t < tt0) idx++;
        let vmin = Infinity, vmax = -Infinity, has = false, qEnd = idx;
        for (let q = idx; q < n && pts[q].t < tt1; q++) {
          const v = pts[q].v;
          if (v < vmin) vmin = v;
          if (v > vmax) vmax = v;
          has = true;
          qEnd = q;
        }
        /* без сглаживаний: левее данных, внутри дыры и правее
         * последней точки — не рисуем совсем */
        if (!has) continue;
        /* дыра в данных (обрыв связи): видимый разрыв линии,
         * а не лживая прямая переброска */
        if (has && lastDataT && qEnd >= 0 &&
            pts[idx].t - lastDataT > CHART_GAP_MS) {
          top.push(NaN, NaN);
          bot.push(NaN, NaN);
        }
        if (qEnd >= 0)
          lastDataT = pts[qEnd].t;
        top.push(px, Y(vmax));
        bot.push(px, Y(vmin));
      }
      /*
       * Конверт — яркая линия реального размаха (как осциллограф),
       * в разреженных местах вырождается в обычную кривую.
       * NaN-маркер = разрыв: перо поднимается.
       */
      let firstX = null, lastX = null;
      for (let i = 0; i < top.length; i += 2)
        if (isFinite(top[i])) {
          if (firstX === null) firstX = top[i];
          lastX = top[i];
        }
      if (firstX !== null) {
        g.beginPath();
        let pen = false;
        for (let i = 0; i < top.length; i += 2) {
          if (!isFinite(top[i])) { pen = false; continue; }
          if (pen) g.lineTo(top[i], top[i + 1]);
          else { g.moveTo(top[i], top[i + 1]); pen = true; }
        }
        pen = false;
        for (let i = bot.length - 2; i >= 0; i -= 2) {
          if (!isFinite(bot[i])) { pen = false; continue; }
          if (pen) g.lineTo(bot[i], bot[i + 1]);
          else { g.moveTo(bot[i], bot[i + 1]); pen = true; }
        }
        g.strokeStyle = col; g.lineWidth = 1; g.stroke();
      }
      if (firstX !== null && lastX !== null) {
        /* заливка — ОТДЕЛЬНЫЙ путь под верхней границей конверта.
         * Разрывы (NaN) НЕ поднимают перо: заливка мостиком через
         * дыру — иначе полигон разваливается на куски */
        g.beginPath();
        let pen = false;
        for (let i = 0; i < top.length; i += 2) {
          if (!isFinite(top[i])) continue;
          if (pen) g.lineTo(top[i], top[i + 1]);
          else { g.moveTo(top[i], top[i + 1]); pen = true; }
        }
        g.lineTo(lastX, ch);
        g.lineTo(firstX, ch);
        g.closePath();
        g.fillStyle = col + '26';
        g.fill();
      }
      g.fillStyle = '#d8dee9'; g.font = '12px ui-monospace,Consolas,monospace';
      g.fillText(pts[n - 1].v.toFixed(3), 8, 12);
      g.fillStyle = '#8b94a7';
      /* max/min = ГРАНИЦЫ ШКАЛЫ: в авто это экстремумы данных,
       * при фиксированной — уставки прибора */
      const showMax = w.auto === false ? hi : maxV;
      const showMin = w.auto === false ? lo : minV;
      g.fillText('max ' + showMax.toFixed(3), cw - 86, 12);
      g.fillText('min ' + showMin.toFixed(3), cw - 86, ch - 4);
    }
    window.addEventListener('resize', chartDrawAll);
    /*
     * RAF-цикл (частота экрана, ~60 Гц): X-окно движется непрерывно,
     * кривая плывёт плавно. Данные добавляются из valuesTick (200мс),
     * отрисовка — на каждом кадре. Стоимость < 1мс на кадр.
     * setTimeout(0) — чтобы let-переменные ниже успели инициализироваться.
     */
    /*
     * RAF-цикл: schedule следующего кадра ПЕРВЫМ (до отрисовки),
     * чтобы даже исключение в chartDrawAll не убило цикл.
     * Плюс setInterval-страховка на случай, если RAF не работает.
     */
    setTimeout(function chartRAF() {
      requestAnimationFrame(chartRAF);
      try { chartDrawAll() } catch (e) { }
    }, 0);
    if (CHARTS_ON)
      setInterval(() => { try { chartDrawAll() } catch (e) { } }, 200);
    let vBuilt = false;
    /*
     * Widgets: the main screen is a list of widget configs kept in
     * localStorage ("widgets"): {k: id, t: chart|val|bits|bar, src: data
     * key, on, min, max}. Base widgets are generated from the MeterData
     * members; "bar" is the torn-stripe progress bar. Drag & drop in the
     * edit mode reorders the array.
     */
    function baseWidgets() {
      /* только структурные виджеты (значения 'm' и CUSTOM); настраиваемые
       * (график/барграф/параметр/бит) пользователь добавляет сам */
      const w = [];
      for (const n of VFLOATS.concat(VUINTS)) w.push({ k: n, t: 'val', src: n, on: true });
      for (const id in CUSTOM) w.push({ k: id, t: 'cval', on: CUSTOM[id].on !== false });
      return w;
    }
    let widgets = null;
    /*
     * Persistence: localStorage is the instant cache, the gateway NVS
     * (via /api/widgets) is the source of truth. rev bumps on every
     * "Сохранить"; a newer server rev wins on the next page load.
     */
    let wRev = parseInt(localStorage.getItem('wrev')) || 0;
    let wDirty = false;
    function saveWidgets() {
      localStorage.setItem('widgets', JSON.stringify(widgets));
      wDirty = true;
      const m = $('wmsg'); if (m) m.textContent = 'не сохранено';
    }
    /*
     * Blob v2: {rev, cur: [...widgets], profiles: {имя: [...]}}.
     * Старый {rev, cfg} мигрирует автоматически. Профили создаёт и
     * применяет страница /panel; здесь мы только сохраняем их как есть.
     */
    let PROFILES = {};
    async function wSave() {
      try {
        const body = JSON.stringify({ rev: wRev + 1, cur: widgets, profiles: PROFILES });
        const r = await (await fetch('/api/widgets', { method: 'POST', body })).json();
        if (!r.ok) { alert('шлюз не смог записать конфиг'); return }
        wRev++; localStorage.setItem('wrev', wRev); wDirty = false;
        const m = $('wmsg'); if (m) m.textContent = 'сохранено (rev ' + wRev + ')';
      } catch (e) { alert('нет ответа от шлюза') }
    }
    async function wSync() {
      try {
        const d = await (await fetch('/api/widgets')).json();
        if (d && d.profiles) PROFILES = d.profiles;
        const cur = d && (d.cur || d.cfg);   /* v2 / старый формат */
        if (Array.isArray(cur) && d.rev > wRev) {
          widgets = cur; wRev = d.rev;
          localStorage.setItem('widgets', JSON.stringify(widgets));
          localStorage.setItem('wrev', wRev);
          pageRender();
        } else if (d.rev === 0 && widgets.length && !wDirty) {
          /* first meet with a blank gateway: migrate the local config */
          const body = JSON.stringify({ rev: 1, cur: widgets, profiles: {} });
          const r = await (await fetch('/api/widgets', { method: 'POST', body })).json();
          if (r.ok) { wRev = 1; localStorage.setItem('wrev', 1) }
        }
      } catch (e) { }
    }
    function loadWidgets() {
      try { widgets = JSON.parse(localStorage.getItem('widgets')) } catch (e) { widgets = null }
      if (!Array.isArray(widgets)) {
        widgets = baseWidgets();
        /* one-time migration: keep the old drag order ("vorder") */
        try {
          const o = JSON.parse(localStorage.getItem('vorder'));
          if (Array.isArray(o))
            widgets.sort((a, b) => (o.indexOf(a.k) + 1 || 1e9) - (o.indexOf(b.k) + 1 || 1e9));
        } catch (e) { }
        saveWidgets();
      }
      /* drop configs whose source disappeared, add new data keys */
      /* migrate the old flow/vol/tot widgets into the unified 'par' */
      const TOT2SRC = { plus: 'tplus', minus: 'tminus', diff: 'total', sum: 'tsum' };
      for (const w of widgets) {
        if (w.t === 'flow') { w.t = 'par'; w.src = 'rate'; w.auto = false }
        else if (w.t === 'vol') { w.t = 'par'; w.src = 'gtotal'; w.auto = false }
        else if (w.t === 'tot') { w.t = 'par'; w.src = TOT2SRC[w.m] || 'total'; w.auto = false }
        /* standalone множитель стал источником 'par' (kf/price) */
        else if (w.t === 'mul') { w.t = 'par'; w.src = w.mode === 'kf' ? 'kf' : 'price'; w.auto = w.auto !== false }
      }
      /* one-time purge: auto-created defaults we shipped earlier */
      const AUTOCREATED = ['rateChart', 'chart_rate', 'bar_qmax', 'par_rate',
        'mul_auto', 'bit_flow', 'flow_l_m', 'vol_l', 'tot_plus_l'];
      widgets = widgets.filter(w => !AUTOCREATED.includes(w.k));
      widgets = widgets.filter(w => w.t === 'bar' || w.t === 'chart' || w.t === 'par' || w.t === 'bit' || w.t === 'btn' || w.t === 'inp' || (w.t === 'cval' && CUSTOM[w.k]) ||
        (w.src && (w.src === 'rate' || VFLOATS.includes(w.src) || VUINTS.includes(w.src))));
      for (const w of widgets) if (w.t === 'btn' && !BTN[w.mode]) w.mode = 'stop';
      for (const w of widgets) if (w.t === 'inp' && !INPS[w.src]) w.src = 'dose';
      for (const w of widgets) if (w.t === 'bar' && !BARM[w.mode]) w.mode = 'qmax';
      for (const w of widgets) if (w.t === 'par') {
        if (!CHSRC[w.src]) w.src = 'rate';
        if (!VOLU[w.vol]) w.vol = 'l';
        if (!PSEC[w.time]) w.time = 'm';
        if (w.auto === undefined) w.auto = true;
      }
      for (const w of widgets)
        if (w.t === 'bit' && (!(w.src in FLAGS) || !(w.b >= 0 && w.b < FLAGS[w.src].length))) { w.src = 'setpoint'; w.b = 0 }
      for (const w of widgets) if (w.t === 'bit' && !/^#[0-9a-fA-F]{6}$/.test(w.col || '')) w.col = BITCOL[0];
      /* chart field sanity (old configs had none of src/per/col/auto) */
      for (const w of widgets)
        if (w.t === 'chart') {
          if (!CHSRC[w.src]) w.src = 'rate';
          if (!CHPER[w.per]) w.per = 600;
          if (!CHCOL.some(x => x.c === w.col)) w.col = CHCOL[5].c;
          if (w.auto === undefined) w.auto = true;
        }
      /* structural widgets (tied to the 'm' data or the CUSTOM block)
       * reappear if missing; the configurable ones (chart/bar/flow/
       * vol/tot/bit/par) are fully user-managed and STAY deleted.
       * A structural widget deleted by the user lands in the hidden
       * list ("whid") and does NOT reappear either */
      const OWNED = ['chart', 'bar', 'flow', 'vol', 'tot', 'bit', 'par', 'btn', 'inp'];
      for (const b of baseWidgets())
        if (!OWNED.includes(b.t) && !wHid().includes(b.k) &&
          !widgets.some(w => w.k === b.k))
          widgets.push(b);
    }
    /* deleted structural widgets (val/cval) stay hidden */
    function wHid() {
      try { return JSON.parse(localStorage.getItem('whid')) || [] } catch (e) { return [] }
    }
    /* короткое имя для СПИСКА панели: без единиц и подробностей -
     * карточка на экране показывает детали, списку они не нужны */
    /* имя в СПИСКЕ виджетов - только по типу, без источников и единиц */
    function listName(w) {
      if (w.t === 'chart') return 'График';
      if (w.t === 'bar') return 'Барграф';
      if (w.t === 'bit') return 'Индикатор';
      if (w.t === 'btn') return BTN[w.mode] ? 'Кнопка ' + BTN[w.mode].n : w.mode;
      if (w.t === 'inp') return INPS[w.src] ? 'Уставка: ' + INPS[w.src].n : w.src;
      return 'Параметр';         // par, val и cval - всё это "параметры"
    }
    function widgetName(w) {
      if (w.t === 'chart') {
        const u = chUnit(w.src);
        return 'График: ' + CHSRC[w.src].n + (u ? ', ' + u : '') +
          ', ' + (CHPER[w.per] || '10 мин');
      }
      if (w.t === 'bar') {
        const m = BARM[w.mode];
        if (!m) return 'Бар: ' + w.mode;
        const u = UN[m.un];
        /* расход считается за период из "Единицы измерения → Период" */
        const suffix = m.un === 'rate'
          ? u + (UN.time ? '/' + UN.time : '')
          : u;
        return 'Бар: ' + m.n + (suffix ? ', ' + suffix : '');
      }
      if (w.t === 'cval') return (CUSTOM[w.k] && CUSTOM[w.k].label) || w.k;
      if (w.t === 'par') {
        const u = parUnit(w);
        return CHSRC[w.src].n + (u ? ', ' + u : '');
      }
      if (w.t === 'bit') return (FLBL[FLAGS[w.src][w.b]] || FLAGS[w.src][w.b]);
      return LBL[w.src] || w.src;
    }
    function widgetHtml(w) {
      if (w.t === 'chart') {
        /* СКОБКИ ОБЯЗАТЕЛЬНЫ: без них второй return становился
         * безусловным и ломал widgetHtml для всех типов виджетов */
        if (!CHARTS_ON)
          /* выключенный график — компактная строка, не огромный canvas */
          return '<div class="vcard chart off" data-k="' + w.k + '"><div class="vname">' + CHSRC[w.src].n + '</div><div class="chartsoff">графики отключены</div></div>';
        return '<div class="vcard chart" data-k="' + w.k + '"><div class="vname">' + CHSRC[w.src].n + (chUnit(w.src) ? ', ' + chUnit(w.src) : '') + ', ' + (CHPER[w.per] || '10 мин') + '</div><canvas id="rc_' + w.k + '"></canvas></div>';
      }
      if (w.t === 'val')
        return '<div class="vcard" data-k="' + w.k + '"><div class="vname">' + (LBL[w.src] || w.src) + '</div><div class="vval" id="vf_' + w.src + '">—</div></div>';
      if (w.t === 'cval') /* custom: src × factor, label from CUSTOM */
        return '<div class="vcard" data-k="' + w.k + '"><div class="vname">' + widgetName(w) + '</div><div class="vval" id="vfc_' + w.k + '">—</div></div>';
      if (w.t === 'par') /* единицы всегда справа ВВЕРХУ, значение вправо */
        return '<div class="vcard" data-k="' + w.k + '"><div class="vname">' + CHSRC[w.src].n +
          '<span class="utop" id="vu_' + w.k + '">' + parUnit(w) + '</span></div>' +
          '<div class="vrow"><span class="vval" id="vp_' + w.k + '">—</span></div></div>';
      if (w.t === 'bit') /* single flag bit as a lamp, label wraps */
        return '<div class="vcard wbit" data-k="' + w.k + '"><div class="vname">' + widgetName(w) + '</div><div class="bitlamp" id="bl_' + w.k + '"></div></div>';
      if (w.t === 'btn') /* квадратная кнопка, подпись в теле */
        return '<div class="vcard wbtn" data-k="' + w.k + '" style="background:' + BTN[w.mode].col +
          '" onclick="btnAct(\'' + w.mode + '\')">' + BTN[w.mode].n + '</div>';
      /* ввод уставки дозатора: значение ВСЕГДА в глобальных единицах
       * прибора (UN.obj), единицы - подсказка справа от поля */
      if (w.t === 'inp') /* единицы всегда справа ВВЕРХУ */
        return '<div class="vcard winp" data-k="' + w.k + '"><div class="vname">' + INPS[w.src].n +
          '<span class="utop" id="iu_' + w.k + '">' + (UN.obj || '') + '</span></div>' +
          '<div class="vrow"><input type="text" inputmode="decimal" id="wi_' + w.k +
          '" value="' + (INP[w.src] ? INP[w.src].v : '') +
          '" onchange="wInpSet(\'' + w.k + '\')" onfocus="this.select()" onkeydown="if(event.key===\'Enter\')this.blur()"></div></div>';
      /* bar: torn-stripe progress bar clamped to min..max, no value text */
      /* барграф: имя слева, уставка-100% с единицами справа,
       * процент - внутри полоски по центру */
      return '<div class="vcard wbar" data-k="' + w.k + '"><div class="vname">' + BARM[w.mode].n +
        '<span class="bsub" id="su_' + w.k + '"></span></div>' +
        '<div class="hbar"><div class="hfill" id="hf_' + w.k + '"></div>' +
        '<span class="bpct" id="bp_' + w.k + '"></span></div></div>';
    }

    /* меню виджетов: два вида (список / добавление), переключаются wView */
    let wView = 'list';
    function renderPanel() {
      const p = $('wlist'); if (!p) return;
      if (wView === 'add') { renderAddView(p); return }
      /* список виджетов: имя + "авто" + ✕, настройки во второй строке */
      let h = '<div class="wrow"><div class="r1"><button id="btnWsave" onclick="wSave()">Сохранить</button>' +
        '<span id="wmsg" class="flash">' + (wDirty ? 'не сохранено' : 'сохранено (rev ' + wRev + ')') + '</span>' +
        '<button onclick="wView=\'add\';renderPanel()" style="margin-left:auto">Добавить</button></div></div>';
      h += '<div class="wsec">Виджеты</div>';
      widgets.forEach((w, i) => {
        /* строка 1: имя + удаление (чекбокса видимости нет);
         * строка 2: элементы настройки */
        /* чекбокс "авто" (par/chart) ВСЕГДА на первой строке, перед ✕ */
        let r1Auto = '';
        if (w.t === 'par') r1Auto = autoChk('wPar', i, w.auto);
        /* график: авто-масштаб нельзя отключить для источников без
         * уставок (к-фактор, цена импульса, частота, импульсы) */
        if (w.t === 'chart')
          r1Auto = CHART_AUTO_ONLY.includes(w.src)
            ? ' <span class="autolbl" style="color:#8b94a7">авто</span>'
            : autoChk('wChart', i, w.auto);
        h += '<div class="wrow"><div class="r1"><span>' + listName(w) + '</span>' + r1Auto +
          ' <button class="warn sq" onclick="wDel(' + i + ')">&#10005;</button></div><div class="r2">';
        /* ── БАРГРАФ ('bar'): источник-режим; 100% = уставка режима ── */
        if (w.t === 'bar') {
          let sb = '<select onchange="wFlow(' + i + ',\'mode\',this.value)">';
          for (const m in BARM) sb += '<option value="' + m + '"' + (w.mode === m ? ' selected' : '') + '>' + BARM[m].n + '</option>';
          h += sb + '</select>';
        }
        /* ── ПАРАМЕТР ('par'): источник + единицы + авто (настройки СКЭ) ── */
        if (w.t === 'par') {
          let ss = '<select onchange="wPar(' + i + ',\'src\',this.value)">';
          for (const s in CHSRC) ss += '<option value="' + s + '"' + (w.src === s ? ' selected' : '') + '>' + CHSRC[s].n + '</option>';
          /* единицы объёма: активны для объёмов и множителя (без "авто");
           * частота (Гц) и импульсы единиц не имеют вовсе */
          const noVol = ['hz', 'none'].includes(CHSRC[w.src].un);
          let sv = '<select' + (w.auto === false && !noVol ? '' : ' disabled') +
            ' onchange="wPar(' + i + ',\'vol\',this.value)">';
          for (const u in VOLU) sv += '<option value="' + u + '"' + (w.vol === u ? ' selected' : '') + '>' + VOLN[u] + '</option>';
          /* период времени активен ТОЛЬКО при источнике "Расход" (без "авто") */
          let st = '<select' + (CHSRC[w.src].un === 'rate' && w.auto === false ? '' : ' disabled') +
            ' onchange="wPar(' + i + ',\'time\',this.value)">';
          for (const u in PSEC) st += '<option value="' + u + '"' + (w.time === u ? ' selected' : '') + '>' + PERN[u] + '</option>';
          h += ss + '</select> ' + sv + '</select> ' + st + '</select>';
        }
        /* ── ИНДИКАТОР ('bit'): один комбобокс всех битов + цвет ── */
        if (w.t === 'bit') {
          h += '<select onchange="wBit(' + i + ',\'bit\',this.value)">' +
            bitOptions(w.src + ':' + w.b) + '</select>' +
            ' <input type="color" value="' + w.col + '" onchange="wBit(' + i + ',\'col\',this.value)">';
        }
        /* ── ГРАФИК ('chart'): источник + окно + цвет + автомасштаб ── */
        if (w.t === 'chart') {
          let ss = '<select onchange="wChart(' + i + ',\'src\',this.value)">';
          for (const s in CHSRC) ss += '<option value="' + s + '"' + (w.src === s ? ' selected' : '') + '>' + CHSRC[s].n + '</option>';
          let sp = '<select onchange="wChart(' + i + ',\'per\',this.value)">';
          for (const p in CHPER) sp += '<option value="' + p + '"' + (w.per == p ? ' selected' : '') + '>' + CHPER[p] + '</option>';
          let sc = '<select onchange="wChart(' + i + ',\'col\',this.value)">';
          for (const c of CHCOL) sc += '<option value="' + c.c + '"' + (w.col === c.c ? ' selected' : '') + ' style="color:' + c.c + '">' + c.n + '</option>';
          h += ss + '</select> ' + sp + '</select> ' + sc + '</select>';
        }
        h += '</div></div>';
      });
      p.innerHTML = h;
    }
    /* чекбокс "авто" для первой строки списка виджетов */
    function autoChk(fn, i, on) {
      const q = String.fromCharCode(39);   /* одинарная кавычка для onchange */
      return ' <label class="autolbl"><input type="checkbox"' + (on !== false ? ' checked' : '') +
        ' onchange="' + fn + '(' + i + ',' + q + 'auto' + q + ',this.checked ? ' + q + '1' + q + ' : ' + q + '0' + q + ')"> авто</label>';
    }
    /* экран добавления: строки по типу, "<" назад к списку */
    function renderAddView(p) {
      let h = '<div class="wrow"><div class="r1"><button onclick="wView=\'list\';renderPanel()">&lt;</button>' +
        '<span>Добавление виджетов</span></div></div>';
      h += '<div class="wrow"><div class="r1"><span>＋ График</span>' +
        ' <label class="autolbl"><input id="ncAuto" type="checkbox" checked> авто</label>' +
        '<button class="sq" onclick="wAddChart()">&#43;</button></div><div class="r2">' +
        '<select id="ncSrc">';
      for (const s in CHSRC) h += '<option value="' + s + '"' + (s === 'rate' ? ' selected' : '') + '>' + CHSRC[s].n + '</option>';
      h += '</select> <select id="ncPer">';
      for (const p in CHPER) h += '<option value="' + p + '"' + (p == 600 ? ' selected' : '') + '>' + CHPER[p] + '</option>';
      h += '</select> <select id="ncCol">';
      for (const c of CHCOL) h += '<option value="' + c.c + '"' + (c.c === CHCOL[5].c ? ' selected' : '') + ' style="color:' + c.c + '">' + c.n + '</option>';
      h += '</select></div></div>';
      h += '<div class="wrow"><div class="r1"><span>＋ Барграф</span><button class="sq" onclick="wAddBar()">&#43;</button></div>' +
        '<div class="r2"><select id="nbMode">';
      for (const m in BARM) h += '<option value="' + m + '"' + (m === 'qmax' ? ' selected' : '') + '>' + BARM[m].n + '</option>';
      h += '</select></div></div>';
      h += '<div class="wrow"><div class="r1"><span>＋ Параметр</span>' +
        ' <label class="autolbl"><input id="npAuto" type="checkbox" checked> авто</label>' +
        '<button class="sq" onclick="wAddPar()">&#43;</button></div>' +
        '<div class="r2"><select id="npSrc">';
      for (const s in CHSRC) h += '<option value="' + s + '"' + (s === 'rate' ? ' selected' : '') + '>' + CHSRC[s].n + '</option>';
      h += '</select> <select id="npVol" disabled><option value="ml">мл</option><option value="l">л</option><option value="m3">м³</option></select>' +
        ' <select id="npTime" disabled><option value="s">с</option><option value="m">мин</option><option value="h">ч</option></select></div></div>';
      h += '<div class="wrow"><div class="r1"><span>＋ Индикатор</span><button class="sq" onclick="wAddBit()">&#43;</button></div>' +
        '<div class="r2"><select id="nbiBit">' + bitOptions('setpoint:0') + '</select>' +
        ' <input id="nbiCol" type="color" value="' + BITCOL[0] + '"></div></div>';
      /* кнопки и уставки - одноразовые: уже добавленные
         ИСЧЕЗАЮТ из набора добавления */
      for (const m in BTN)
        if (!widgets.some(w => w.t === 'btn' && w.mode === m))
          h += '<div class="wrow" title="' + BTN[m].h + '"><div class="r1"><span style="color:' + BTN[m].col + '">＋ Кнопка ' + BTN[m].n +
            '</span><button class="sq" onclick="wAddBtn(\'' + m + '\')">&#43;</button></div>' +
            '<div class="r2"><span class="flash">' + BTN[m].h + '</span></div></div>';
      /* ввод уставок дозатора: Доза / Упреждение / Перелив */
      for (const s in INPS)
        if (!widgets.some(w => w.t === 'inp' && w.src === s))
          h += '<div class="wrow"><div class="r1"><span>＋ Ввод: ' + INPS[s].n +
            '</span><button class="sq" onclick="wAddInp(\'' + s + '\')">&#43;</button></div>' +
            '<div class="r2"><span class="flash">уставка дозатора</span></div></div>';
      p.innerHTML = h;
      npMirror();
    }
    /* все биты всех групп одним списком: value "группа:номер",
     * метка "Группа · Имя флага" (имена повторяются между группами) */
    function bitOptions(cur) {
      /* в названии - только имя флага, без группы */
      let s = '';
      for (const g in FLAGS)
        FLAGS[g].forEach((f, bi) => {
          s += '<option value="' + g + ':' + bi + '"' + (cur === g + ':' + bi ? ' selected' : '') +
            '>' + (FLBL[f] || f) + '</option>';
        });
      return s;
    }
    /* строка "＋ Параметр": при "авто" комбобоксы ПОКАЗЫВАЮТ единицы
     * прибора (заблокированные), при снятии - разблокируются с них */
    function npMirror() {
      const auto = $('npAuto').checked, src = $('npSrc').value;
      const un = CHSRC[src] ? CHSRC[src].un : 'rate';
      const noVol = ['hz', 'none'].includes(un);
      if (auto) {
        if (un === 'kf' || un === 'price') {
          /* множитель: единицы из меню Множитель → Единицы */
          if (UNKEY[MULU]) $('npVol').value = UNKEY[MULU];
        } else {
          const u = UN[un];
          if (u && UNKEY[u]) $('npVol').value = UNKEY[u];
        }
        if (un === 'rate' && UNKEY[UN.time]) $('npTime').value = UNKEY[UN.time];
      }
      $('npVol').disabled = auto || noVol;
      $('npTime').disabled = auto || un !== 'rate';
    }
    /* общие операции панели: показ, правка поля, удаление */
    function wFlow(i, which, v) { widgets[i][which] = v; saveWidgets(); pageRender() }
    function wDel(i) {
      const w = widgets[i];
      if (w && (w.t === 'val' || w.t === 'cval')) {
        /* structural: remember the deletion so it does not resurrect */
        const hid = wHid();
        if (!hid.includes(w.k)) { hid.push(w.k); localStorage.setItem('whid', JSON.stringify(hid)) }
      }
      widgets.splice(i, 1); saveWidgets(); pageRender();
    }
    /* настройка поля графика (источник/окно/цвет/автомасштаб); смена
     * окна сбрасывает историю - у окон разная частота выборки */
    function wChart(i, which, v) {
      if (which === 'per') widgets[i].per = parseInt(v)
      else if (which === 'auto') widgets[i].auto = v === '1';
      else widgets[i][which] = v;
      saveWidgets(); pageRender();
    }
    /* создатели новых виджетов, по одному на тип */
    const CHART_LIMIT = 3;         /* сервер имеет 3 ring-буфера */
    function wAddChart() {         /* 'chart' — график (макс. 3) */
      if (widgets.filter(w => w.t === 'chart').length >= CHART_LIMIT) {
        alert('Максимум ' + CHART_LIMIT + ' графиков');
        return;
      }
      widgets.push({
        k: 'chart' + Date.now(), t: 'chart', src: $('ncSrc').value,
        per: parseInt($('ncPer').value), col: $('ncCol').value,
        auto: $('ncAuto').checked, on: true
      });
      saveWidgets(); pageRender();
    }
    function wAddBar() {           /* 'bar'  — барграф */
      widgets.push({ k: 'bar' + Date.now(), t: 'bar', mode: $('nbMode').value, on: true });
      saveWidgets(); pageRender();
    }
    function wAddPar() {           /* 'par'  — параметр */
      const w = {
        k: 'par' + Date.now(), t: 'par', src: $('npSrc').value,
        vol: $('npVol').value, time: $('npTime').value,
        auto: $('npAuto').checked, on: true
      };
      /* с "авто" виджет добавляется с единицами ПРИБОРА */
      parSyncAuto(w);
      widgets.push(w);
      saveWidgets(); pageRender();
    }
    /* настройка поля "параметра" (источник/единицы/период/авто) */
    function wPar(i, which, v) {
      if (which === 'auto') widgets[i].auto = v === '1';
      else widgets[i][which] = v;
      saveWidgets(); pageRender();
    }
    function wAddBtn(mode) {       /* 'btn' — кнопка СТОП/СТАРТ/ПАУЗА/СБРОС */
      widgets.push({ k: 'btn' + Date.now(), t: 'btn', mode, on: true });
      saveWidgets(); pageRender();
    }
    function wAddInp(src) {        /* 'inp' — ввод уставки дозатора */
      widgets.push({ k: 'inp' + Date.now(), t: 'inp', src, on: true });
      saveWidgets(); pageRender();
    }
    function wAddBit() {           /* 'bit'  — индикатор бита */
      const [g, bi] = $('nbiBit').value.split(':');
      widgets.push({
        k: 'bit' + Date.now(), t: 'bit', src: g,
        b: parseInt(bi) || 0, col: $('nbiCol').value, on: true
      });
      saveWidgets(); pageRender();
    }
    function wBit(i, which, v) {
      if (which === 'bit') {       /* combined "группа:номер" combo */
        const [g, bi] = v.split(':');
        widgets[i].src = g; widgets[i].b = parseInt(bi) || 0;
      } else widgets[i][which] = v;
      saveWidgets(); pageRender();
    }
    document.addEventListener('change', e => {
      if (e.target.id === 'npSrc' || e.target.id === 'npAuto') npMirror();
    });

    /* setpoints for the bar 100% levels: floats from the settings tree;
     * "Qmax,л" is matched by name anywhere, the rest strictly inside the
     * "Уставки" submenu (those names repeat across the tree) */
    async function paramsTick() {
      try {
        const d = await (await fetch('/api/params')).json();
        SP = {};
        const un = { rate: '', obj: '', ob: '', time: '' };
        const mult = { type: MULT.type, value: MULT.value };
        let mulU = MULU;
        const btnp = { mode: BTNP.mode, reset: BTNP.reset };
        const inp = {};
        for (const p of d.params) {
          if (!p.n) continue;
          /* doser buttons: Режим Стоп|Старт + CMD Сброс объёма */
          if (p.t === 11 && p.tb === 'Дозатор' && p.n === 'Режим' &&
            p.o && p.o.includes('Старт')) btnp.mode = p.i;
          if (p.cx && p.n.indexOf('Сброс') === 0 && p.tb === 'Сброс объёма') btnp.reset = p.i;
          /* doser setpoints: Доза / Упреждение / Перелив in Уставки */
          if (p.t === 0 && p.tb === 'Уставки') {
            if (p.n === 'Доза') inp.dose = { id: p.i, v: parseFloat(p.v) };
            if (p.n === 'Упреждение') inp.pre = { id: p.i, v: parseFloat(p.v) };
            if (p.n === 'Перелив') inp.over = { id: p.i, v: parseFloat(p.v) };
            /* нижняя уставка расхода: min фиксированной шкалы графика */
            if (p.n === 'Расход-') SP['rate-'] = parseFloat(p.v);
          }
          /* метод линеаризации: активен => kf динамический (parValue) */
          if (p.tb === 'Линеаризация' && p.n === 'Метод') LINM = p.v;
          /* setpoints: floats for the bar 100% levels */
          if (p.t === 0 && p.w)
            for (const m in BARM)
              if (p.n === BARM[m].sp && (m === 'qmax' || p.tb === 'Уставки'))
                SP[m] = parseFloat(p.v);
          /* units: enums in "Основные → Единицы измерения" (Расход/Объём/
           * Общий + Период: за какое время считается расход) */
          if (p.t === 11 && p.tb === 'Единицы измерения') {
            /* enum-значение может прийти текстом или (после чьей-то
             * кривой установки) индексом — лечим оба случая */
            const ev = UNSHORT[p.v] || (p.o && p.o.length ? (UNSHORT[p.o[+p.v]] || p.o[+p.v]) : '') || '';
            const tv = PERSHORT[p.v] || (p.o && p.o.length ? (PERSHORT[p.o[+p.v]] || p.o[+p.v]) : '') || '';
            if (p.n === 'Расход') un.rate = ev;
            if (p.n === 'Объём') un.obj = ev;
            if (p.n === 'Общий') un.ob = ev;
            if (p.n === 'Период') un.time = tv;
          }
          /* multiplier: "Тип" (bool) + "Значение" (float) + "Единицы"
           * (enum мл/л/м³) in Множитель */
          if (p.tb === 'Множитель') {
            if (p.t === 10 && p.n === 'Тип')
              mult.type = (p.r === 0 || p.v === 'K-фактор') ? 'kf' : 'price';
            if (p.t === 0 && p.n === 'Значение')
              mult.value = parseFloat(p.v);
            if (p.t === 11 && p.n === 'Единицы')
              mulU = UNSHORT[p.v] || '';
          }
        }
        if (mult.type !== MULT.type || mult.value !== MULT.value || mulU !== MULU) {
          MULT = mult;
          MULU = mulU;
          pageRender();               /* relabel the multiplier widgets */
        }
        if (un.rate !== UN.rate || un.obj !== UN.obj || un.ob !== UN.ob ||
          un.time !== UN.time) {
          UN = un;
          /* авто-параметры зеркалят новые единицы прибора */
          for (const w of widgets) parSyncAuto(w);
          pageRender();               /* relabel the bars with the new units */
        }
        /* кнопки и уставки дозатора: перерисовать при изменении */
        const inpChanged = Object.keys(INP).length !== Object.keys(inp).length ||
          Object.keys(inp).some(s => INP[s] && inp[s] && (INP[s].id !== inp[s].id || INP[s].v !== inp[s].v));
        if (btnp.mode !== BTNP.mode || btnp.reset !== BTNP.reset) BTNP = btnp;
        if (inpChanged) { INP = inp; pageRender() }
      } catch (e) { }
    }

/* страница-хук: на главной это полная перерисовка экрана (vBuild),
 * на /widgets - только перерисовка меню */
function pageRender() { (window.vBuild || renderPanel)() }
