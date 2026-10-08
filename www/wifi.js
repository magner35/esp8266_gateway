        let NETS = [];
        async function wifiLoad() {
            /* /scan блокирующий: отвечает готовым списком (~2 с) */
            const box = document.getElementById('nets');
            box.innerHTML = '<p class="scanmsg">сканирование…</p>';
            try {
                const r = await (await fetch('/scan')).json();
                NETS = r.nets || [];
            } catch (e) {
                NETS = [];
            }
            if (!NETS.length) {
                box.innerHTML = '<p class="scanmsg">сети не найдены' +
                    (r.err ? ' (ошибка скана ' + r.err + ')' : '') +
                    ' — <button class="pick" onclick="wifiLoad()">повторить скан</button>' +
                    '<br>Сеть можно ввести вручную в поле ниже</p>';
                return;
            }
            let h = '<table>';
            NETS.forEach((n, i) => {
                h += '<tr><td>' + esc(n.s) + '</td><td class="dbm">' + n.r + ' dBm</td><td>' +
                    (n.e ? '🔒' : '') + '</td><td><button class="pick" onclick="pick(' + i + ')">выбрать</button></td></tr>';
            });
            box.innerHTML = h + '</table>';
        }
        function pick(i) { document.getElementById('ssid').value = NETS[i].s }
        wifiLoad();
        async function forgetWifi() {
            if (!confirm('Стереть сохранённые сети и перезапустить шлюз ' +
                         'в режиме точки доступа?')) return;
            try { await fetch('/api/forgetwifi', { method: 'POST' }); } catch (e) { }
            document.body.innerHTML = '<p class="scanmsg">Сети стёрты, шлюз ' +
                'перезагружается в режим точки доступа. Подключитесь к ' +
                'SKE02-GW-XXXX (открытая) и откройте 192.168.4.1</p>';
        }
