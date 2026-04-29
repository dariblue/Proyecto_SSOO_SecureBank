# Guía de Uso — SecureBank Fase II (Servidor TCP)

Esta guía explica cómo compilar, ejecutar y utilizar las nuevas funcionalidades implementadas en la **Fase II**. El sistema ahora funciona como un servidor TCP distribuido.

## 1. Preparación y Compilación

### Requisitos
*   Sistema operativo Linux (o Docker en Windows/Mac).
*   Compilador `gcc`, `make`.
*   Librería POSIX RealTime (`-lrt`).

### Compilación
Usa el Makefile incluido para limpiar y compilar todos los componentes:
```bash
make clean && make
```
Esto generará los ejecutables: `banco`, `usuario`, `monitor` e `init_cuentas`.

---

## 2. Ejecución con Docker (Recomendado)

Debido a que el puerto 5000 suele estar ocupado en macOS (por AirPlay Receiver), se recomienda usar Docker para aislar el entorno.

1.  **Construir la imagen**:
    ```bash
    docker build -t securebank .
    ```
2.  **Lanzar el contenedor** (sin mapear puerto 5000 al host para evitar conflictos):
    ```bash
    docker run -it --rm -v "$(pwd):/securebank" securebank
    ```
3.  **Dentro del contenedor**, inicializa y lanza el banco:
    ```bash
    ./init_cuentas
    ./banco
    ```

---

## 3. Uso del Cliente (Telnet)

Una vez que el servidor `./banco` esté corriendo, puedes conectar clientes desde otras terminales (dentro del mismo contenedor o red).

### Conexión
```bash
telnet 127.0.0.1 5000
```

### Flujo de Autenticación
Al conectar, el sistema te pedirá tu número de cuenta:
*   **Login**: Introduce un ID existente (ej: `1001`).
*   **Crear cuenta**: Introduce `0`. El sistema te pedirá el nombre del titular y te asignará un nuevo ID automáticamente.
*   **Salir**: Introduce `-1`.

### Operaciones
El menú es idéntico al de la Fase I, pero ahora es **asíncrono**. Puedes realizar depósitos, retiros y transferencias. El servidor no se bloquea mientras esperas hilos de ejecución.

---

## 4. Pruebas de Seguridad y Robustez

### Validación de Fraude (Bloqueo)
Para verificar que el sistema de seguridad (Fix #2) funciona:
1.  Conecta un cliente vía Telnet.
2.  Realiza 3 retiros rápidos consecutivos.
3.  El **Monitor** detectará la anomalía.
4.  El **Banco** enviará una orden de `BLOQUEO` por el pipe exclusivo de ese proceso.
5.  El cliente recibirá el mensaje de alerta y su conexión TCP se cerrará automáticamente.

### Verificación de Zombis (Fix #1)
Mientras el servidor corre, puedes verificar que no se acumulan procesos finalizados:
```bash
ps aux | grep usuario
```
Si el Cosechador de Zombis funciona, solo verás los procesos `usuario` que tengan una conexión Telnet activa.

---

## 5. Suite de Tests Automatizada

Se ha incluido un script para validar toda la Fase II de un solo golpe:
```bash
chmod +x test_fase2.sh
./test_fase2.sh
```
El script verifica compilación, concurrencia, protocolos de red, detección de fraude y limpieza de recursos.
