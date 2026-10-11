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

    async function ntpSave() {
      const body = 'on=' + ($('ntpOn').checked ? 1 : 0) +
          'auto=' + ($('ntpAuto').checked ? 1 : 0) +
        '&s1=' + encodeURIComponent($('ntpS1').value.trim()) +
        '&s2=' + encodeURIComponent($('ntpS2').value.trim()) +
        '&tz=' + encodeURIComponent($('ntpTz').value || 0);
      try {
        const r = await (await fetch('/api/ntp', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body
        })).json();
        ntpShow(r);
        const m = $('ntpMsg');
        m.textContent = 'сохранено';
        setTimeout(() => { if (m.textContent === 'сохранено') m.textContent = '' }, 2500);
      } catch (e) {
        $('ntpMsg').textContent = 'нет ответа от шлюза';
      }
    }

    ntpLoad();
    /* обновлять статус времени, пока страница открыта */
    setInterval(async () => {
      try { ntpShow(await (await fetch('/api/ntp')).json()) } catch (e) { }
    }, 5000);
