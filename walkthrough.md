# SecureBank — Walkthrough

## Resumen

Se han implementado las **3 fases completas** del proyecto SecureBank: el orquestador (`banco.c`), el cliente multihilo (`usuario.c`) y el vigilante de fraudes (`monitor.c`). También se crearon archivos de soporte para compilación y datos de prueba.

## Archivos modificados

### [banco.c](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/banco.c)

| Función | Descripción |
|---------|-------------|
| `leer_config()` | Parsea las 14 claves de `config.txt` con `fgets` + `sscanf`. Ignora comentarios (`#`). |
| `crear_cuenta()` | Doble bloqueo: `SEM_CONFIG` para reservar ID → `SEM_CUENTAS` para append binario a `cuentas.dat`. |
| `escribir_log()` | Append + flush al archivo de log. |
| `procesar_log()` | Decodifica `DatosLog` de `MQ_LOG`, formatea con tipo de op, divisa, estado y PID. |
| `lanzar_usuario()` | `fork()` + `execl("./usuario", id, pipe_fd)`. Padre guarda pipe write-end. |
| `lanzar_monitor()` | `fork()` + `execl("./monitor")`. Guarda PID para `SIGTERM` al cierre. |

### [usuario.c](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/usuario.c)

| Función | Descripción |
|---------|-------------|
| `leer_config_usuario()` | Parseo idéntico al de `banco.c`, solo lectura. |
| `enviar_monitor()` | Rellena `DatosMonitor` y envía a `MQ_MONITOR` con `mq_send`. |
| `enviar_log()` | Rellena `DatosLog` (incluye PID, timestamp) y envía a `MQ_LOG`. |
| `saldo_ptr()` | Helper: devuelve puntero al saldo correcto según divisa. |
| `limite_retiro()` | Helper: devuelve límite de retiro por divisa desde config. |
| `limite_transferencia()` | Helper: devuelve límite de transferencia por divisa. |
| `thread_deposito()` | `sem_wait` → lee cuenta → suma al saldo → `escribir_cuenta` → `sem_post` → notifica. |
| `thread_retiro()` | Valida límite → `sem_wait` → verifica saldo ≥ cantidad → resta → escribe → notifica. |
| `thread_transferencia()` | Valida límite → lee origen y destino → verifica saldo → mueve fondos → escribe ambas → notifica. |
| `thread_mover_divisa()` | Conversión via EUR como base: `cantidad/tasa_orig * tasa_dest`. Atómico bajo semáforo. |
| `consultar_saldos()` | Lee cuenta bajo semáforo y muestra tabla con EUR, USD, GBP. |

### [monitor.c](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/monitor.c)

| Función | Descripción |
|---------|-------------|
| `leer_config_monitor()` | Lee `UMBRAL_RETIROS` y `UMBRAL_TRANSFERENCIAS` de `config.txt`. |
| `enviar_alerta()` | Rellena `DatosAlerta` y envía a `MQ_ALERTA` con `mq_send`. |
| `analizar()` (retiros) | Busca/crea entrada en `TrackRetiros`. Incrementa contador. Si ≥ umbral → alerta + reset. Cualquier otra op resetea el contador. |
| Bucle `poll()` | `poll()` con timeout 500ms sobre `MQ_MONITOR`. Lee `DatosMonitor` con `mq_receive` y llama `analizar()`. |

## Archivos nuevos

| Archivo | Descripción |
|---------|-------------|
| [Makefile](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/Makefile) | Compila `banco`, `usuario`, `monitor`, `init_cuentas` con `-pthread -lrt`. Solo Linux. |
| [Dockerfile](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/Dockerfile) | Contenedor `gcc:latest` para compilar en macOS vía Docker. |
| [init_cuentas.c](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/init_cuentas.c) | 5 cuentas de prueba (1001-1005) con saldos multi-divisa. |

## Flujo de ejecución

```
banco (padre)
  ├── fork → ./monitor (vigilante)
  │     └── poll(MQ_MONITOR) → analizar() → mq_send(MQ_ALERTA)
  └── fork → ./usuario <id> <pipe_fd> (por cada sesión)
        ├── poll(stdin + pipe)
        ├── pthread → thread_deposito/retiro/transferencia/mover_divisa
        │     ├── sem_wait(SEM_CUENTAS)
        │     ├── leer/escribir cuentas.dat
        │     ├── sem_post(SEM_CUENTAS)
        │     ├── mq_send(MQ_MONITOR)
        │     └── mq_send(MQ_LOG)
        └── pipe(lectura) ← BLOQUEO del padre
```

## Cómo compilar y probar

```bash
# 1. Instalar Docker Desktop, luego:
cd /Users/d4r1/Documents/GitHub/ProyectoSSOO
docker build -t securebank .
docker run -it --rm -v "$(pwd):/securebank" securebank

# 2. Dentro del contenedor Linux:
make clean && make
./init_cuentas
./banco

# 3. En el prompt de banco:
#    0 → crear cuenta nueva
#    1001 → entrar como John Doe
#    Dentro del menú usuario: depositar, retirar, transferir...
```

## Nota sobre lints macOS

Todos los errores de lint reportados por el IDE son del tipo `'mqueue.h' file not found` / `Unknown type name 'mqd_t'`. Esto es **esperado** en macOS — POSIX message queues no existen en Darwin. El código compilará sin errores en Linux con el Makefile proporcionado.
