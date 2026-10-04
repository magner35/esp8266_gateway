    /* на этой странице нет главного экрана: перерисовывается только меню */
    loadWidgets();
    renderPanel();
    wSync();
    paramsTick(); setInterval(paramsTick, 30000);
  
