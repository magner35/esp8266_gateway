    const COMMON = '\x01';              /* pseudo tab for params directly in the section */
    let P = [], SECT = [], built = '', cur = 0, curT = {}, curS = {}, pwMap = {}, dirty = new Set();
    const p2 = v => String(v).padStart(2, '0');
    /* manual refresh only: values never change under the user's fingers */
    let retryT = 0;
    async function tick() {
      try {
        const d = await (await fetch('/api/params')).json();
        P = d.params; SECT = d.sections; dirty = new Set();
        setRender(d.count + '|' + d.params.length + '|' + d.sections.length + '|' + d.rc);
        applyFilter();
        /* the tree is empty while a rescan/listing is in flight on the
         * gateway - poll instead of demanding a wiring check */
        if (!P.length && !retryT)
          retryT = setTimeout(() => { retryT = 0; tick() }, 3000);
      } catch (e) { }
    }
    /* the per-section password field follows the active section */
    function pwInput() { pwMap[cur] = $('secPw').value }
    function syncPw() { $('secPw').value = pwMap[cur] || '' }
    function mark(i) {
      dirty.add(i);
      const tr = document.querySelector('tr[data-i="' + i + '"]');
      if (tr) tr.className = 'dirty';
      const w = $('w' + i);            /* grid cells have no tr: mark the input */
      if (w) w.classList.add('dirty');
    }
    function unmark(i) {
      const tr = document.querySelector('tr[data-i="' + i + '"]');
      if (tr) tr.className = '';
      const w = $('w' + i);
      if (w) w.classList.remove('dirty');
    }
    /* edit widget html for the parameter type (no per-row apply button:
     * edits accumulate and are sent by the section-wide "Применить") */
    function widget(q) {
      if (!q.w) return '';
      const i = q.i, m = ' oninput="mark(' + i + ')" onchange="mark(' + i + ')"';
      if ((q.t === 10 || q.t === 11) && q.o && q.o.length) {
        let h = '<select id="w' + i + '"' + m + '>';
        q.o.forEach((s, k) => h += '<option value="' + k + '"' + (String(k) === String(q.r) ? ' selected' : '') + '>' + esc(s) + '</option>');
        return '<div class="set">' + h + '</select></div>';
      }
      if (q.m) return '<div class="set"><input id="w' + i + '" type="password" maxlength="6" inputmode="numeric" placeholder="******"' + m + '></div>';
      if (q.t === 0) return '<div class="set"><input id="w' + i + '" type="text" class="fnum" inputmode="decimal" maxlength="12" title="' + esc((q.lo || '?') + ' .. ' + (q.hi || '?')) + '" value="' + esc(q.v) + '"' + m + '></div>';
      if (q.t === 1) return '<div class="set"><input id="w' + i + '" type="text" maxlength="6" inputmode="numeric" title="' + esc((q.lo || '?') + ' .. ' + (q.hi || '?')) + '" value="' + esc(q.v) + '"' + m + '></div>';
      if (q.t === 2 || q.t === 3) {
        const m2 = q.v.split(/[:.]/);
        if (q.t === 2) return '<div class="set"><input class="t2" id="wh' + i + '" type="number" min="0" max="23" step="1" value="' + esc(m2[0]) + '"' + m + '><span class="dv">:</span><input class="t2" id="wm' + i + '" type="number" min="0" max="59" step="1" value="' + esc(m2[1]) + '"' + m + '></div>';
        return '<div class="set"><input class="t2" id="wd' + i + '" type="number" min="1" max="31" step="1" value="' + esc(m2[0]) + '"' + m + '><span class="dv">.</span><input class="t2" id="wm2' + i + '" type="number" min="1" max="12" step="1" value="' + esc(m2[1]) + '"' + m + '><span class="dv">.</span><input class="t2" id="wy' + i + '" type="number" min="0" max="99" step="1" value="' + esc(m2[2]) + '"' + m + '></div>';
      }
      const st = ' min="' + (q.lo !== '' ? esc(q.lo) : '0') + '" max="' + (q.hi !== '' ? esc(q.hi) : '65535') + '"';
      return '<div class="set"><input id="w' + i + '" type="number" step="1"' + st + ' value="' + esc(q.v) + '"' + m + '></div>';
    }
    /* one table row: name | editor (RO values as text, CMD items as a button) */
    function row(q, where) {
      let ctrl;
      if (q.cx) ctrl = '<button class="run" onclick="runParam(' + q.i + ')">Выполнить</button>';
      else ctrl = q.w ? widget(q) : '<span class="val' + (q.a > 90 ? ' stale' : '') + '" id="v' + q.i + '">' + esc(q.v) + '</span>';
      return '<tr data-n="' + esc((q.n + ' ' + q.i).toLowerCase()) + '" data-i="' + q.i + '"><td>' + esc(q.n || ('P' + q.i)) +
        (where ? '<div class="where">' + esc(where) + '</div>' : '') + '</td><td class="ctl">' + ctrl + '</td></tr>';
    }
    const THEAD = '<tr><th>Параметр</th><th></th></tr>';
    /*
     * The server names the owning L2 menu explicitly ("tb"), because the
     * firmware prints subtrees before the parent's own params and the id
     * order alone cannot recover the hierarchy. L3 = the nearest submenu
     * when its level is exactly 3.
     */
    function decorate(arr) {
      const rows = [];
      for (const q of arr) {
        rows.push({ q, l2: q.tb || COMMON, l3: (q.gl === 3 && q.g) ? q.g : '' });
      }
      return rows;
    }
    let curTab = COMMON;                 /* active L2 tab name of the section */
    function setRender(fp) {
      if (!P.length) { $('tabs').innerHTML = ''; $('subtabs').innerHTML = ''; $('sections').innerHTML = '<p>Опрос прибора… если через минуту пусто — проверьте подключение UART и питание, затем нажмите «Обновить».</p>'; built = fp; return }
      let h = '';
      SECT.forEach((s, k) => h += '<button class="' + (k === cur ? 'act' : '') + '" onclick="go(' + k + ')">' + esc(s) + '</button>');
      $('nav').innerHTML = h;
      if (cur >= SECT.length) cur = 0;
      syncPw();
      const rows = decorate(P.filter(q => q.s === cur || SECT.length <= 1));
      const tabs = [...new Set(rows.map(r => r.l2))];
      if (!(cur in curT) || curT[cur] >= tabs.length) curT[cur] = 0;
      const t = tabs[curT[cur]];
      curTab = t;
      const tr = rows.filter(r => r.l2 === t);
      const subs = [...new Set(tr.map(r => r.l3))];
      const sk = cur + '|' + t;
      if (!(sk in curS) || curS[sk] >= subs.length) curS[sk] = 0;
      const s = subs[curS[sk]];
      h = '';
      tabs.forEach((n, k) => h += '<button class="' + (n === t ? 'act' : '') + '" onclick="go2(' + k + ')">' + esc(n === COMMON ? 'Общие' : n) + '</button>');
      $('tabs').innerHTML = '<div class="tabs">' + h + '</div>';
      h = '';
      if (subs.length > 1 || subs[0] !== '')
        subs.forEach((n, k) => h += '<button class="' + (n === s ? 'act' : '') + '" onclick="go3(' + k + ')">' + esc(n || 'Общие') + '</button>');
      $('subtabs').innerHTML = h ? '<div class="subtabs">' + h + '</div>' : '';
      const vis = tr.filter(r => r.l3 === s);
      /*
       * Linearization directions ("Прямое/Реверс направление") hold the F/V
       * calibration pairs: render them as an F|V grid, one pair per row
       * (F01|V01, F02|V02, ...), instead of a long two-column list.
       */
      const linRe = /^[FV]\d\d/;
      if (vis.length >= 4 && vis.every(r => linRe.test(r.q.n))) {
        const F = {}, V = {};
        for (const r of vis) (r.q.n[0] === 'F' ? F : V)[r.q.n.slice(1, 3)] = r.q;
        const keys = [...new Set([...Object.keys(F), ...Object.keys(V)])].sort();
        const cell = q => q ? '<label>' + esc(q.n) + '</label>' + widget(q) : '<label></label><span></span>';
        h = '<div class="lingrid">';
        for (const k of keys) h += cell(F[k]) + cell(V[k]);
        h += '</div>';
      } else {
        h = '<table>' + THEAD;
        for (const r of vis) h += row(r.q);
        h += '</table>';
      }
      $('sections').innerHTML = h; built = fp;
    }
    /* search view: filter matches anywhere, across all sections and tabs */
    function renderSearch(f) {
      let h = '<table>' + THEAD;
      for (const q of P) {
        if (!(q.n + ' ' + q.i).toLowerCase().includes(f)) continue;
        const sec = q.s >= 0 && q.s < SECT.length ? SECT[q.s] : '';
        h += row(q, sec ? (sec + (q.g ? ' · ' + q.g : '')) : '');
      }
      h += '</table>';
      $('sections').innerHTML = h || '<p>Ничего не найдено.</p>';
      $('tabs').innerHTML = ''; $('subtabs').innerHTML = '';
    }
    function go(k) { cur = k; setRender(built); syncPw(); applyFilter() }
    function go2(k) { curT[cur] = k; applyFilter() }
    function go3(k) { curS[cur + '|' + curTab] = k; applyFilter() }
    function applyFilter() {
      const f = $('filter').value.trim().toLowerCase();
      if (!f) { setRender(built); return }
      renderSearch(f);
    }
    function curVal(q) {
      const i = q.i, g = x => $(x + i);
      if (q.t === 10 || q.t === 11) return g('w').value;
      if (q.t === 2) return p2(g('wh').value) + ':' + p2(g('wm').value);
      if (q.t === 3) return p2(g('wd').value) + '.' + p2(g('wm2').value) + '.' + p2(g('wy').value);
      return g('w').value;
    }
    function flash(msg) {
      $('applyMsg').textContent = msg;
      setTimeout(() => { if ($('applyMsg').textContent === msg) $('applyMsg').textContent = '' }, 2500);
    }
    /* apply every edited parameter, unlocking its menu with the section
     * password first; the first failure aborts the rest */
    async function applyAll() {
      const ids = [...dirty];
      if (!ids.length) { flash('нет изменений'); return }
      $('btnApply').disabled = true;
      let done = 0;
      for (const i of ids) {
        const q = P.find(x => x.i === i);
        if (!q) continue;
        const body = 'id=' + i + '&v=' + encodeURIComponent(curVal(q)) + '&pw=' + encodeURIComponent(pwMap[q.s] || '');
        try {
          const r = await (await fetch('/api/set', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body })).json();
          if (!r.ok) { alert('«' + (q.n || ('P' + i)) + '»: ' + r.error); break }
          dirty.delete(i); done++;
          unmark(i);
          /* keep the local cache in sync with the device read-back, so a
           * tab switch re-renders the new value, not the pre-edit one */
          if (r.value !== undefined) {
            q.v = r.value;
            if ((q.t === 10 || q.t === 11) && q.o)
              q.r = q.o.findIndex(s => s === r.value);
          }
          if (q.m) { const w = $('w' + i); if (w) w.value = '' }
        } catch (e) { alert('нет ответа от шлюза'); break }
      }
      $('btnApply').disabled = false;
      if (done) flash('применено: ' + done);
    }
    /* run a menu command item ('x'), unlocked with its section password */
    async function runParam(i) {
      const q = P.find(x => x.i === i);
      if (!q || !q.cx) return;
      if (!confirm('Выполнить «' + (q.n || ('P' + i)) + '»?')) return;
      try {
        const r = await (await fetch('/api/run', {
          method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'id=' + i + '&pw=' + encodeURIComponent(pwMap[q.s] || '')
        })).json();
        if (!r.ok) alert('«' + (q.n || ('P' + i)) + '»: ' + r.error);
        else flash('команда выполнена');
      } catch (e) { alert('нет ответа от шлюза') }
    }
    /* "Обновить": full 'l' listing on the device (settings poll is on-demand
     * only), then the page reloads from the refreshed cache */
    async function refreshAll() {
      if (dirty.size && !confirm('Есть несохранённые изменения. Обновить без применения?')) return;
      const b = $('btnRefresh'); b.disabled = true; b.textContent = 'Обновление…';
      try {
        const r = await (await fetch('/api/refresh', { method: 'POST' })).json();
        if (!r.ok) alert('Прибор не ответил на запрос листинга');
        await tick();
      } catch (e) { alert('нет ответа от шлюза') }
      b.disabled = false; b.textContent = 'Обновить';
    }
    tick();
  
