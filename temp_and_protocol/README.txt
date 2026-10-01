====================================================================
  Ejemplo simple — BME280 con el COMPONENTE esp-idf-lib/bmp280
  ESP32-C6 DevKit + módulo Techno-Innov (I2C). main.c muy corto.
  Compatible con ESP-IDF 5.3 o posterior (incluye la 6.1).
====================================================================

QUÉ ES
  La versión sencilla: la biblioteca hace todo el trabajo del sensor. El
  programa lee temperatura, humedad y presión, y las imprime cada 2 s.
  La carpeta ejemplo_nodo_sensor_bme280/ hace lo mismo con código propio,
  sin descargar nada; sirve para ver cómo funciona el sensor por dentro.

CABLEADO
  GND -> GND    3,3 V -> 3V3 (nunca 5 V)    SDA -> GPIO6    SCL -> GPIO7

COMPILAR Y GRABAR
  Cargar el entorno de ESP-IDF (export.sh en Linux/macOS, o el terminal
  ESP-IDF en Windows) y, desde esta carpeta, ejecutar:
      idf.py set-target esp32c6
      idf.py build          (la primera vez descarga la biblioteca: Internet)
      idf.py -p PUERTO flash monitor        (para salir del monitor: Ctrl + ])

SALIDA ESPERADA
  I (xxx) bme: Sensor: BME280 (con humedad)
  I (xxx) bme: T=24.13 C  HR=47.8 %  P=1012.4 hPa
  ... (cada 2 s, sin interrupciones)


DOS PROBLEMAS FRECUENTES, YA RESUELTOS EN ESTE EJEMPLO

  1) "error 259 (ESP_ERR_INVALID_STATE)" y "bmp280: Sensor not found"
     Es un NACK: el sensor no responde porque i2cdev no activa las pull-ups
     internas por defecto. La solución, ya incluida en main.c antes de
     bmp280_init, es:
         dev.i2c_dev.cfg.sda_pullup_en = true;
         dev.i2c_dev.cfg.scl_pullup_en = true;
         dev.i2c_dev.cfg.master.clk_speed = 100000;

  2) "hace dos lecturas y luego no imprime más"
     Ocurre cuando una lectura devuelve error y el programa no lo comprueba:
     el bucle se queda mudo. En este ejemplo el bucle comprueba el valor de
     retorno de la lectura y continúa aunque una falle (imprime un aviso en
     lugar de detenerse). Con las pull-ups y los 100 kHz, además, las lecturas
     no deberían fallar.


SI PERSISTE EL FALLO
  - Reducir la velocidad a 50 kHz:  dev.i2c_dev.cfg.master.clk_speed = 50000;
    La combinación i2cdev 2.x + driver I2C nuevo es reciente y a veces resulta
    quisquillosa a 100 kHz con cables largos.
  - Para una opción robusta que no dependa de la biblioteca ni de Internet,
    está la carpeta ejemplo_nodo_sensor_bme280/ (código propio con el driver
    nuevo). Hace exactamente lo mismo.
  - Si no aparece el puerto al grabar, el sospechoso habitual es el cable USB:
    muchos solo llevan alimentación. Conviene probar con otro cable.
