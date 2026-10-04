    const $ = i => document.getElementById(i);
    function hms(t) { const s = t % 60, m = (t / 60 | 0) % 60, h = t / 3600 | 0; return h ? h + 'ч ' + m + 'м' : m ? m + 'м ' + s + 'с' : s + 'с' }

    /*
     * Конфиг виджетов лежит в шлюзе (/api/widgets) блобом v2:
     *   {rev, cur: [...widgets], profiles: {имя: [...]}}.
     * Эта страница управляет профилями; cur применяется одним кликом.
     */
    let blob = { rev: 0, cur: null, profiles: {} };

    async function load() {
      try {
        const d = await (await fetch('/api/widgets')).json();
        blob = { rev: d.rev || 0, cur: d.cur || d.cfg || null, profiles: d.profiles || {} };
      } catch (e) { }
      render();
    }

    function render() {
      let h = '';
      const names = Object.keys(blob.profiles);
      if (!names.length) h = '<div class="prow"><span class="pname" style="color:#8b94a7">профилей нет</span></div>';
      for (const n of names) {
        h += '<div class="prow"><span class="pname">' + esc(n) +
          ' <span style="color:#8b94a7">(' + blob.profiles[n].length + ' видж.)</span></span>' +
          '<button class="ok" onclick="profApply(\'' + esc(n) + '\')">Применить</button>' +
          '<button class="warn" onclick="profDel(\'' + esc(n) + '\')">Удалить</button></div>';
      }
      $('profiles').innerHTML = h;
    }

    function esc(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])) }

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
      if (!Array.isArray(blob.cur)) { alert('текущий набор ещё не сохранён в шлюз'); return }
      blob.profiles[n] = blob.cur;
      blob.rev++;
      if (await post()) { $('newName').value = ''; render() }
    }

    /* применить профиль: он становится текущим набором (rev++), главная
     * страница подтянет его при следующей загрузке/сохранении */
    async function profApply(n) {
      if (!confirm('Применить профиль «' + n + '»? Текущий набор будет заменён.')) return;
      blob.cur = blob.profiles[n];
      blob.rev++;
      if (await post()) render();
    }

    async function profDel(n) {
      if (!confirm('Удалить профиль «' + n + '»?')) return;
      delete blob.profiles[n];
      blob.rev++;
      if (await post()) render();
    }

    /* отладочная панель */
    async function infoTick() {
      try {
        const r = await (await fetch('/api/info')).json();
        $('fw').textContent = r.fw + ' · параметров: ' + r.count;
        $('ip').textContent = r.ip;
        $('rss').textContent = r.ap ? r.rssi + ' dBm' : r.ssid + ': ' + r.rssi + ' dBm';
        $('upt').textContent = 'uptime ' + hms(r.uptime);
        $('heap').textContent = 'heap ' + (r.heap / 1024 | 0) + ' KiB';
      } catch (e) { }
    }
    async function rescan() { await fetch('/api/rescan', { method: 'POST' }); infoTick() }
    function rebootDev() { if (confirm('Перезагрузить прибор?')) fetch('/api/reboot', { method: 'POST' }) }
    infoTick(); setInterval(infoTick, 5000);
    load();
  
