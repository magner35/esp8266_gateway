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
    /* ---- обновление с сервера производителя (upd.c) ---- */
    /* один вход обновления: пакет по имени файла
     *   system_full*.skpak -> /api/sys/upload (шлюз+прибор)
     *   gateway*.skpak     -> /api/ota/upload (шлюз)
     *   meter или ske02 .skpak -> /api/avr/upload (прибор) */
    function fwUploadAuto(inp) {
      const f = inp.files[0];
      const m = document.getElementById('fwMsg');
      if (!f) return;
      const nm = f.name.toLowerCase();
      let url = '/api/avr/upload';       /* meter/ske02 - по умолчанию */
      if (nm.indexOf('system_full') === 0) url = '/api/sys/upload';
      else if (nm.indexOf('gateway') === 0) url = '/api/ota/upload';
      m.className = 'msg';
      m.textContent = 'загрузка… 0%';
      const xhr = new XMLHttpRequest();
      xhr.open('POST', url);
      xhr.upload.onprogress = e => {
        if (e.lengthComputable)
          m.textContent = 'загрузка… ' + Math.round(e.loaded / e.total * 100) + '%';
      };
      xhr.onload = () => {
        try {
          const r = JSON.parse(xhr.responseText);
          if (r.ok) {
            m.textContent = '';
            document.getElementById('otaBtn').disabled = !(
              url === '/api/sys/upload' || url === '/api/ota/upload');
            document.getElementById('avrBtn').disabled = !(
              url === '/api/sys/upload' || url === '/api/avr/upload');
          }
          else { m.className = 'msg err'; m.textContent = 'ошибка: ' + (r.error || '?') }
        } catch (e) { m.className = 'msg err'; m.textContent = 'ошибка ответа' }
      };
      xhr.onerror = () => { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' };
      xhr.send(f);
    }

    async function avrFlash(btn) {
      const m = document.getElementById('fwMsg');
      if (!confirm('Обновить ПО? Обмен данными остановится на минуту.')) return;
      btn.disabled = true;
      m.className = 'msg';
      m.textContent = 'загрузка... ждите';
      try {
        const pb = document.getElementById('avrPb');
        pb.style.width = '0';
        /* Шлюз ГАСИТ WiFi на время сессии: реальный done/total браузеру
         * недоступен до самого конца. Полосу ведут ЧАСЫ (отдельный
         * тикер - висячий fetch при погашенном WiFi молчит до
         * TCP-таймаутов, catch не срабатывает). Раскладка как в
         * реальных фазах @250К: запись - основная часть до 90%,
         * верификация и финальные операции - остаток. Курс - вперёд. */
        let shown = 0, done = false;
        const t0 = Date.now();
        const move = p => {
          if (!done && p > shown) {
            shown = p;
            pb.style.width = Math.min(100, Math.round(shown)) + '%';
          }
        };
        const byClock = () => {
          const t = (Date.now() - t0) / 1000;
          if (t < 3) move(t / 3 * 6);                     /* сброс+sync */
          else if (t < 8) move(6 + (t - 3) / 5 * 84);     /* запись->90% */
          else if (t < 18) move(90 + (t - 8) / 10 * 7);   /* verify->97% */
          else move(97);                                   /* ждём ответ */
        };
        const step = r => {
          if (r.total > 0 && r.done > 0) {
            if (r.phase === 2) move(r.done / r.total * 90);
            else if (r.phase === 3) move(90 + r.done / r.total * 7);
          }
          if (r.phase === 3) move(90);
        };
        const tick = setInterval(byClock, 300);
        try {
          await fetch('/api/avr/flash', { method: 'POST' });
        } catch (e) { }
        for (;;) {
          await new Promise(r => setTimeout(r, 700));
          let r;
          try {
            r = await (await fetch('/api/avr/flash', { cache: 'no-store' })).json();
          } catch (e) { continue; } /* WiFi погашен - ждём возврата */
          step(r);
          if (!r.running) {
            done = true;
            clearInterval(tick);
            if (r.ok) { m.textContent = 'готово: ' + r.kb + ' КБ, проверка ок'; pb.style.width = '100%' }
            else { m.className = 'msg err'; m.textContent = 'ошибка прошивки' }
            break;
          }
        }
      } catch (e) { m.className = 'msg err'; m.textContent = 'нет ответа от шлюза' }
      btn.disabled = false;
    }

    /* ---- OTA прошивки шлюза ---- */
    async function otaFlash(btn) {
      const m = document.getElementById('fwMsg');
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
