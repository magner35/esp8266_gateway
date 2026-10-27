    /* Сервис: перезапуск прибора + NTP-настройки шлюза */
    function rebootDev() {
      if (confirm('Перезагрузить прибор?'))
        fetch('/api/reboot', { method: 'POST' });
    }

    async function ntpLoad() {
      try {
        const r = await (await fetch('/api/ntp')).json();
        $('ntpOn').checked = !!r.on;
        $('ntpS1').value = r.s1 || '';
        $('ntpS2').value = r.s2 || '';
          $('ntpTz').value = (r.tz !== undefined) ? r.tz : 3;
        $('ntpAuto').checked = !!r.auto;
        ntpAutoChk();
        ntpShow(r);
      } catch (e) { }
    }

    /* строка периодического статуса времени убрана из интерфейса */
    function ntpShow(r) { void r; }

    /* при "Авто" поле пояса неактивно: значение придёт с IP-геолокации */
    function ntpAutoChk() {
      const a = $('ntpAuto');
      if (a) $('ntpTz').disabled = a.checked;
    }

    /* Сохранить: сохранить настройки -> дождаться синхронизации ->
     * отправить TIME в прибор. Неудача = короткое сообщение, TIME не шлём */
    async function ntpSave() {
      const m = $('ntpMsg');
      try {
        await fetch('/api/ntp', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'on=' + ($('ntpOn').checked ? 1 : 0) +
            '&auto=' + ($('ntpAuto').checked ? 1 : 0) +
            '&s1=' + encodeURIComponent($('ntpS1').value.trim()) +
            '&s2=' + encodeURIComponent($('ntpS2').value.trim()) +
            '&tz=' + encodeURIComponent($('ntpTz').value || 0)
        });
      } catch (e) { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза'; return }
      if (!$('ntpOn').checked) { m.className = 'msg'; m.textContent = ''; return }
      /* с "Авто" полный цикл = sync + определение пояса по IP;
       * пока оба не готовы - TIME не шлём */
      const wantAuto = $('ntpAuto').checked;
      m.className = 'msg';
      m.textContent = 'синхронизация…';
      let synced = false, tzok = false;
      for (let i = 0; i < 15; i++) {
        await new Promise(r => setTimeout(r, 1000));
        try {
          const st = await (await fetch('/api/ntp')).json();
          synced = !!st.sync;
          tzok = !!st.tzok;
        } catch (e) { }
        if (synced && (!wantAuto || tzok)) break;
        if (synced && wantAuto && !tzok) m.textContent = 'определяем часовой пояс…';
      }
      if (!synced) {
        m.className = 'msg err';
        m.textContent = 'серверы времени не ответили, не синхронизировано';
        return;
      }
      if (wantAuto && !tzok) {
        m.className = 'msg err';
        m.textContent = 'часовой пояс не определён, не синхронизировано';
        return;
      }
      try {
        const r = await (await fetch('/api/ntp/push', { method: 'POST' })).json();
        if (r.ok) { m.className = 'msg'; m.textContent = 'синхронизировано' }
        else { m.className = 'msg err'; m.textContent = 'не синхронизировано: ' + r.error }
      } catch (e) { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' }
    }

    ntpLoad();
    /* при каждом заходе в раздел Сервис - перечитать конфиг из шлюза:
     * настройки живут в NVS ESP, не в браузере */
    const inSvc = () => location.hash === '#service';
    if (inSvc()) ntpLoad();
    window.addEventListener('hashchange', () => { if (inSvc()) ntpLoad() });
    /* ---- прошивка прибора (urboot) ---- */
    /* загрузка стартует сразу по выбору файла; прогресс - в строке
     * над кнопкой (XHR: fetch прогресс отправки не показывает) */
    function avrUploadAuto(inp) {
      const f = inp.files[0];
      const m = document.getElementById('avrMsg');
      if (!f) return;
      m.className = 'msg';
      m.textContent = 'подготовка… 0%';
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/avr/upload');
      xhr.upload.onprogress = e => {
        if (e.lengthComputable)
          m.textContent = 'подготовка… ' + Math.round(e.loaded / e.total * 100) + '%';
      };
      xhr.onload = () => {
        try {
          const r = JSON.parse(xhr.responseText);
          if (r.ok) { m.textContent = ''; document.getElementById('avrBtn').disabled = false }
          else { m.className = 'msg err'; m.textContent = 'ошибка: ' + (r.error || '?') }
        } catch (e) { m.className = 'msg err'; m.textContent = 'ошибка ответа' }
      };
      xhr.onerror = () => { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' };
      xhr.send(f);
    }

    async function avrFlash(btn) {
      const m = document.getElementById('avrMsg');
      if (!confirm('Обновить ПО? Обмен данными остановится на минуту.')) return;
      btn.disabled = true;
      m.className = 'msg';
      m.textContent = 'загрузка... ждите';
      try {
        const pb = document.getElementById('avrPb');
        pb.style.width = '0';
        /* БЕЗ отката назад: общий прогресс по фазам (запись 0..90%,
         * верификация 60..100%), курс держим только вперёд */
        let shown = 0;
        const step = r => {
          let p = 0;
          if (r.total > 0 && r.done > 0) {
            if (r.phase === 2) p = r.done / r.total * 90;
            else if (r.phase === 3) p = 90 + r.done / r.total * 40;
          }
          if (r.phase === 3 && r.done === 0) p = 90;
          if (p > shown) shown = p;
          pb.style.width = Math.min(100, Math.round(shown)) + '%';
        };
        await fetch('/api/avr/flash', { method: 'POST' });
        for (;;) {
          await new Promise(r => setTimeout(r, 1000));
          let r;
          try { r = await (await fetch('/api/avr/flash')).json() } catch (e) { break }
          step(r);
          if (!r.running) {
            if (r.ok) { m.textContent = 'готово: ' + r.kb + ' КБ, проверка ок'; pb.style.width = '100%' }
            else { m.className = 'msg err'; m.textContent = 'ошибка прошивки' }
            break;
          }
        }
      } catch (e) { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' }
      btn.disabled = false;
    }

    /* ВРЕМЕННО: сырой hex без .skpak (кнопка восстановления) */
    function avrUploadHex(inp) {
      const f = inp.files[0];
      const m = document.getElementById('avrMsg');
      if (!f) return;
      m.className = 'msg';
      m.textContent = 'hex… 0%';
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/avr/uploadhex');
      xhr.upload.onprogress = e => {
        if (e.lengthComputable)
          m.textContent = 'hex… ' + Math.round(e.loaded / e.total * 100) + '%';
      };
      xhr.onload = () => {
        try {
          const r = JSON.parse(xhr.responseText);
          if (r.ok) { m.textContent = ''; document.getElementById('avrBtn').disabled = false }
          else { m.className = 'msg err'; m.textContent = 'ошибка: ' + (r.error || '?') }
        } catch (e) { m.className = 'msg err'; m.textContent = 'ошибка ответа' }
      };
      xhr.onerror = () => { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' };
      xhr.send(f);
    }

    /* ---- OTA прошивки шлюза ---- */
    function otaUploadAuto(inp) {
      const f = inp.files[0];
      const m = document.getElementById('otaMsg');
      if (!f) return;
      m.className = 'msg';
      m.textContent = 'загрузка… 0%';
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota/upload');
      xhr.upload.onprogress = e => {
        if (e.lengthComputable)
          m.textContent = 'загрузка… ' + Math.round(e.loaded / e.total * 100) + '%';
      };
      xhr.onload = () => {
        try {
          const r = JSON.parse(xhr.responseText);
          if (r.ok) { m.textContent = ''; document.getElementById('otaBtn').disabled = false }
          else { m.className = 'msg err'; m.textContent = 'ошибка: ' + (r.error || '?') }
        } catch (e) { m.className = 'msg err'; m.textContent = 'ошибка ответа' }
      };
      xhr.onerror = () => { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' };
      xhr.send(f);
    }

    async function otaFlash(btn) {
      const m = document.getElementById('otaMsg');
      if (!confirm('Прошить шлюз? Он перезагрузится, страница подождёт его возвращения.')) return;
      btn.disabled = true;
      m.className = 'msg';
      m.textContent = 'прошиваем… шлюз перезагружается';
      try { await fetch('/api/ota/flash', { method: 'POST' }) } catch (e) { }
      for (let i = 0; i < 30; i++) {
        await new Promise(r => setTimeout(r, 2000));
        try {
          const r = await (await fetch('/api/info', { cache: 'no-store' })).json();
          if (r && r.gw) { m.textContent = 'готово, шлюз на связи'; break }
        } catch (e) { }
        m.textContent = 'ждём шлюз… ' + ((i + 1) * 2) + ' с';
      }
      btn.disabled = false;
    }
