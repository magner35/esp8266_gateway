        let NETS = [];
        async function wifiLoad() {
            /* /scan блокирующий (~2-6 с): запускается при ЗАХОДЕ
             * в раздел Wi-Fi (не при загрузке всей страницы) */
            const box = document.getElementById('nets');
            box.innerHTML = '<p class="scanmsg">сканирование…</p>';
            let r = null;
            try {
                r = await (await fetch('/scan')).json();
                NETS = r.nets || [];
            } catch (e) {
                NETS = [];
            }
            if (!NETS.length) {
                box.innerHTML = '<p class="scanmsg">сети не найдены' +
                    (r && r.err ? ' (ошибка скана ' + r.err + ')' : '') +
                    ' — <button class="pick" onclick="wifiLoad()">повторить скан</button>' +
                    '<br>Сеть можно ввести вручную в поле ниже</p>';
                return;
            }
            let h = '<table>';
            NETS.forEach((n, i) => {
                h += '<tr><td>' + esc(n.s) + '</td><td class="dbm">' + n.r + ' dBm</td><td>' +
                    (n.e ? '🔒' : '') + '</td><td><button class="pick" onclick="pick(' + i + ')">Выбрать</button></td></tr>';
            });
            box.innerHTML = h + '</table>';
        }
        function pick(i) { document.getElementById('ssid').value = NETS[i].s }
        /* скан запускать при каждом входе в раздел Wi-Fi */
        const inWifi = () => location.hash === '#wifi' || location.hash === '';
        if (inWifi()) wifiLoad();
        window.addEventListener('hashchange', () => { if (inWifi()) wifiLoad() });
        async function forgetWifi() {
            if (!confirm('Стереть ВСЕ сохранённые сети и перезапустить шлюз ' +
                         'в режиме точки доступа?')) return;
            try { await fetch('/api/forgetwifi', { method: 'POST' }); } catch (e) { }
            document.body.innerHTML = '<p class="scanmsg">Сети стёрты, шлюз ' +
                'перезагружается в режим точки доступа. Подключитесь к ' +
                'SKE02-GW-XXXX (открытая) и откройте 192.168.4.1</p>';
        }
        /* --- сохранённые сети (Просмотр) --- */
        async function loadSaved() {
            const box = document.getElementById('savedlist');
            let nets = [];
            try {
                const r = await (await fetch('/api/wifilist')).json();
                nets = r.nets || [];
            } catch (e) { }
            if (!nets.length) {
                box.innerHTML = '<p class="scanmsg">нет сохранённых сетей</p>';
                return;
            }
            let h = '<table>';
            nets.forEach(n => {
                h += '<tr><td>' + esc(n.s) + '</td><td style="text-align:right">' +
                     '<button class="delbtn" onclick="delNet(' + n.i + ')" ' +
                     'title="Удалить эту сеть">✕</button></td></tr>';
            });
            box.innerHTML = h + '</table>';
        }
        async function delNet(i) {
            if (!confirm('Удалить эту сеть из сохранённых?')) return;
            try { await fetch('/api/wifidel?i=' + i, { method: 'POST' }); } catch (e) { }
            loadSaved();
        }
        function toggleSaved() {
            const el = document.getElementById('saved');
            const show = el.style.display === 'none' || !el.style.display;
            el.style.display = show ? 'block' : 'none';
            if (show) loadSaved();
        }
