        let NETS = [];
        async function wifiLoad() {
            /* /scan блокирующий: отвечает готовым списком (~2 с) */
            const r = await (await fetch('/scan')).json();
            NETS = r.nets || [];
            let h = '<table>';
            NETS.forEach((n, i) => {
                h += '<tr><td>' + esc(n.s) + '</td><td class="dbm">' + n.r + ' dBm</td><td>' +
                    (n.e ? '🔒' : '') + '</td><td><button class="pick" onclick="pick(' + i + ')">выбрать</button></td></tr>';
            });
            document.getElementById('nets').innerHTML = h + '</table>';
        }
        function pick(i) { document.getElementById('ssid').value = NETS[i].s }
        wifiLoad();
    
