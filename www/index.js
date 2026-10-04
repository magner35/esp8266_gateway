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
    document.addEventListener('drop', e => {
      const c = e.target.closest && e.target.closest('.vcard');
      if (!editing || !dragK || !c || c.dataset.k === dragK) return;
      e.preventDefault();
      const src = document.querySelector('.vcard[data-k="' + dragK + '"]');
      if (!src) return;
      /* insert before/after by the pointer half of the target card */
      const r = c.getBoundingClientRect();
      const after = (e.clientY - r.top) > r.height / 2 || (e.clientX - r.left) > r.width / 2;
      c.parentNode.insertBefore(src, after ? c.nextSibling : c);
      /* the DOM order of the enabled cards IS the widget order */
      const ord = [...$('values').children].map(x => x.dataset.k);
      widgets.sort((a, b) =>
        (ord.indexOf(a.k) + 1 || 1e9) - (ord.indexOf(b.k) + 1 || 1e9));
      saveWidgets();
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
       * выбранные единицы (объём, для расхода ещё и период) */
      for (const w of widgets || []) {
        if (w.t !== 'par' || !w.on) continue;
        const e = $('vp_' + w.k), v = parValue(w, d);
        if (e && v !== undefined && isFinite(v))
          e.textContent = Number(v.toPrecision(4))
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
        const v = m ? d[m.src] : undefined;
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
  
