# SecureBank — Phase 1: Motor de Arranque e Inicialización

## Contexto

Arquitectura confirmada: 3 ejecutables (`banco`, `usuario`, `monitor`) comunicados via POSIX mqueue + pipes + semáforos nombrados. Los archivos de trabajo están en `/Users/d4r1/Documents/GitHub/ProyectoSSOO/` (directorio raíz, NO en `SecureBank-main/`).

> [!CAUTION]
> **macOS NO soporta `mqueue.h` (colas de mensajes POSIX).** La API `mq_open`, `mq_send`, `mq_receive` no existe en Darwin/macOS. El `checker` proporcionado es un binario **Linux ARM64** (ELF aarch64), lo cual confirma que el proyecto **debe compilarse y ejecutarse en Linux**.
>
> No tienes Docker instalado. Necesitas una de estas opciones:
> 1. **Instalar Docker Desktop** y usar el Dockerfile que proporciono abajo
> 2. **Usar un servidor Linux** (ej. lab de la universidad, VM con UTM/Parallels)
>
> **Recomiendo Docker** — es la forma más rápida. Un solo `docker build + docker run` y listo.

## Cambios realizados en banco.c (ya aplicados)

### `leer_config()` — Parseo completo de config.txt
- Lee las 14 claves del archivo: `PROXIMO_ID`, `LIM_RET_{EUR,USD,GBP}`, `LIM_TRF_{EUR,USD,GBP}`, `UMBRAL_RETIROS`, `UMBRAL_TRANSFERENCIAS`, `NUM_HILOS`, `ARCHIVO_CUENTAS`, `ARCHIVO_LOG`, `CAMBIO_USD`, `CAMBIO_GBP`
- Ignora comentarios (`#`) y líneas cortas
- Imprime resumen de la configuración cargada

### `crear_cuenta()` — Creación segura con doble semáforo
1. **`SEM_CONFIG`** → lock → asigna `proximo_id`, incrementa, persiste en `config.txt` → unlock
2. **`SEM_CUENTAS`** → lock → escribe struct `Cuenta` en `cuentas.dat` (append binario) → unlock
3. Registra la operación en el log

### `escribir_log()` — Escritura en archivo de log
- Abre en modo append, escribe la línea, flush+close

### `procesar_log()` — Decodificación de mensajes MQ_LOG
- Formatea cada `DatosLog` recibido y lo escribe al archivo de log con tipo de operación, monto, divisa, estado y PID

### Forward declaration fix
- `escribir_log()` se llama desde `crear_cuenta()` que aparece antes en el archivo. Necesitamos mover su definición antes o añadir un forward declaration.

## Propuesta: Archivos de build

### [NEW] [Makefile](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/Makefile)
- Compila los 3 binarios + `init_cuentas`
- Flags: `-Wall -Wextra -pthread -lrt` (Linux requiere `-lrt` para mqueue)
- Target `clean` para limpiar binarios

### [NEW] [Dockerfile](file:///Users/d4r1/Documents/GitHub/ProyectoSSOO/Dockerfile)
- Basado en `gcc:latest` (Debian/Linux ARM64 en M4)
- Monta el código como volumen
- Permite compilar y ejecutar en un entorno Linux completo

## Plan de ejecución

1. **Instalar Docker Desktop** (si no está instalado)  
2. Crear `Dockerfile` y `Makefile`  
3. Añadir forward declaration de `escribir_log` en `banco.c`
4. Compilar dentro del contenedor con `make`
5. Ejecutar `./banco` dentro del contenedor para validar Fase 1

## Verificación

### Dentro del contenedor Docker:
```bash
make clean && make
./init_cuentas        # Crear cuentas.dat con datos de prueba
./banco               # Debe mostrar config cargada, permitir crear cuentas
```

### Comprobaciones:
- La configuración se imprime correctamente al arrancar
- Se puede crear una cuenta nueva (opción 0) y se escribe en `cuentas.dat`
- El log registra la creación de la cuenta
- `config.txt` se actualiza con el nuevo `PROXIMO_ID`
