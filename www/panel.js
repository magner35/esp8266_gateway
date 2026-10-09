
    /*
     * Конфиг виджетов лежит в шлюзе (/api/widgets) блобом v2:
     *   {rev, cur: [...widgets], profiles: {имя: [...]}}.
     * Эта страница управляет профилями; cur применяется одним кликом.
     */
    let blob = { rev: 0, cur: null, profiles: {} };

    /* аптайм секунды -> "1д 02:03:04" */
    function hms(s) {
      s |= 0;
      const d = s / 86400 | 0, h = s % 86400 / 3600 | 0,
            m = s % 3600 / 60 | 0, sec = s % 60;
      const two = n => String(n).padStart(2, '0');
      return (d ? d + 'д ' : '') + two(h) + ':' + two(m) + ':' + two(sec);
    }

    async function panelLoad() {
      try {
        const d = await (await fetch('/api/widgets')).json();
        blob = { rev: d.rev || 0, cur: d.cur || d.cfg || null, profiles: d.profiles || {} };
      } catch (e) { }
      profRender();
    }

    function profRender() {
      let h = '';
      const names = Object.keys(blob.profiles);
      if (!names.length) h = '<div class="prow"><span class="pname" style="color:#8b94a7">профилей нет</span></div>';
      for (const n of names) {
        h += '<div class="prow"><span class="pname">' + esc(n) +
          ' <span style="color:#8b94a7">(' + blob.profiles[n].length + ' видж.)</span></span>' +
          '<button class="ok" onclick="profApply(\'' + profEsc(n) + '\')">Применить</button>' +
          '<button class="warn" onclick="profDel(\'' + profEsc(n) + '\')">Удалить</button></div>';
      }
      $('profiles').innerHTML = h;
    }

    function profEsc(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])) }

    async function post() {
      const r = await (await fetch('/api/widgets', { method: 'POST', body: JSON.stringify(blob) })).json();
      if (!r.ok) alert('шлюз не смог записать конфиг');
      return r.ok;
    }

    /* создать профиль из ТЕКУЩЕГО набора на главном экране */
    async function profCreate() {
      const n = $('newName').value.trim();
      if (!n) { alert('введите имя профиля'); return }
      if (blob.profiles[n] && !confirm('Профиль «' + n + '» уже есть. Перезаписать?')) return;
      /* «текущий» = ЖИВАЯ раскладка главной страницы (общая для всего
       * SPA): она новее серверской копии, пока там не нажали «Сохранить».
       * Заодно она становится и серверным cur — как при применении профиля */
      let cur = (typeof widgets !== 'undefined' && Array.isArray(widgets) && widgets.length)
        ? JSON.parse(JSON.stringify(widgets)) : blob.cur;
      if (!Array.isArray(cur)) { alert('набор виджетов пуст'); return }
      blob.cur = cur;
      blob.profiles[n] = JSON.parse(JSON.stringify(cur));
      blob.rev++;
      if (await post()) { $('newName').value = ''; profRender() }
    }

    /* применить профиль: он становится текущим набором НЕМЕДЛЕННО -
     * и на сервере, и в живой раскладке главной (массив widgets общий
     * для SPA). Иначе главная показывала бы старый набор, а её
     * «Сохранить» молча затирало бы применение профиля */
    async function profApply(n) {
      if (!confirm('Применить профиль «' + n + '»? Текущий набор будет заменён.')) return;
      const cur = JSON.parse(JSON.stringify(blob.profiles[n]));
      blob.cur = cur;
      blob.rev++;
      if (await post()) {
        widgets = JSON.parse(JSON.stringify(cur));
        wRev = blob.rev; wDirty = false;
        localStorage.setItem('widgets', JSON.stringify(widgets));
        localStorage.setItem('wrev', wRev);
        if (typeof pageRender === 'function') pageRender();
        profRender();
      }
    }

    async function profDel(n) {
      if (!confirm('Удалить профиль «' + n + '»?')) return;
      delete blob.profiles[n];
      blob.rev++;
      if (await post()) profRender();
    }

    /* отладочная панель */
    async function infoTick() {
      try {
        const r = await (await fetch('/api/info')).json();
        $('fw').textContent = r.fw + ' · параметров: ' + r.count;
        $('ip').textContent = r.ip;
        $('rss').textContent = r.ap ? r.rssi + ' dBm' : r.ssid + ': ' + r.rssi + ' dBm';
        $('upt').textContent = hms(r.uptime);
        $('heap').textContent = (r.heap / 1024 | 0) + ' KiB';
      } catch (e) { }
    }
    async function rescan() { await fetch('/api/rescan', { method: 'POST' }); infoTick() }
    function rebootDev() { if (confirm('Перезагрузить прибор?')) fetch('/api/reboot', { method: 'POST' }) }
    infoTick(); setInterval(infoTick, 5000);
    panelLoad();
  
