        let NETS = [];
        async function wifiLoad(restart) {
            if (restart) await fetch('/scan?restart=1');
            const r = await (await fetch('/scan')).json();
            if (r.running) { setTimeout(() => load(0), 1500); return }
            NETS = r.nets;
            let h = '<table>';
            NETS.forEach((n, i) => {
                h += '<tr><td>' + esc(n.s) + '</td><td class="dbm">' + n.r + ' dBm</td><td>' +
                    (n.e ? '🔒' : '') + '</td><td><button class="pick" onclick="pick(' + i + ')">выбрать</button></td></tr>';
            });
            document.getElementById('nets').innerHTML = h + '</table>';
        }
        function pick(i) { document.getElementById('ssid').value = NETS[i].s }
        wifiLoad(1);
    
