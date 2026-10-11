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

    /* локальное время шлюза = utc + tz; без синхронизации utc - это
     * аптайм с 1970, показываем только статус */
    function ntpShow(r) {
      const st = $('ntpSt');
      if (!st) return;
      if (!r.on) { st.innerHTML = 'синхронизация выключена'; return }
      if (r.auto && !r.tzok) { st.innerHTML = 'определяем часовой пояс по IP…'; return }
      if (!r.sync) { st.innerHTML = 'ждём ответа сервера…'; return }
      const d = new Date((r.utc + (r.tz || 0) * 3600) * 1000);
      const two = n => String(n).padStart(2, '0');
      st.innerHTML = 'синхронизировано: <b>' +
        two(d.getUTCDate()) + '.' + two(d.getUTCMonth() + 1) + '.' + d.getUTCFullYear() +
        ' ' + two(d.getUTCHours()) + ':' + two(d.getUTCMinutes()) + ':' + two(d.getUTCSeconds()) +
        '</b> (UTC' + (r.tz >= 0 ? '+' : '') + r.tz + (r.auto ? ', пояс по IP' : '') + ')';
    }

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
    /* обновлять статус времени, пока страница открыта */
    setInterval(async () => {
      try { ntpShow(await (await fetch('/api/ntp')).json()) } catch (e) { }
    }, 5000);
