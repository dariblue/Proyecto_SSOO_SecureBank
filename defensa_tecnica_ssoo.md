# SecureBank — Documento de Defensa Técnica (Fase II)

## 1. Visión General del Sistema

SecureBank es un sistema bancario concurrente que funciona como una **arquitectura cliente-servidor distribuida** implementada íntegramente en C con APIs POSIX de Linux. El proyecto se compone de **cuatro ejecutables independientes** que se comunican mediante mecanismos de IPC (Inter-Process Communication):

| Ejecutable | Rol | IPC que usa |
|---|---|---|
| `banco` | Servidor TCP principal. Orquesta todo. | Sockets, Pipes, Message Queues, Semáforos |
| `usuario` | Un proceso por cliente conectado. Gestiona la sesión. | Message Queues, Pipes, Semáforos |
| `monitor` | Vigilante independiente de fraude. | Message Queues |
| `init_cuentas` | Utilidad única para crear la BD inicial. | Ficheros binarios |

**La gran diferencia entre Fase I y Fase II**: En la Fase I el banco era interactivo por terminal local. En la Fase II se convirtió en un **servidor TCP concurrente** que puede manejar múltiples clientes remotos conectados simultáneamente vía `telnet`.

---

## 2. Arquitectura de Ficheros e IPC

```
                 ┌─────────────────────────────────────┐
                 │           banco.c (Padre)           │
                 │                                     │
                 │  socket() → bind() → listen()       │
                 │  poll([server_fd, MQ_LOG, MQ_ALERT])│
                 └────────┬──────────┬─────────────────┘
                          │ fork()   │ fork()
              ┌───────────▼──┐  ┌───▼──────────┐
              │  usuario.c   │  │  monitor.c   │
              │  (hijo/TCP)  │  │  (vigilante) │
              └──────┬───────┘  └──────┬───────┘
                     │                 │
           ┌─────────┴──┐    ┌─────────┴────────┐
           │  MQ_LOG    │    │   MQ_ALERTA      │
           │(hijo→padre)│    │(monitor→padre)   │
           └────────────┘    └──────────────────┘
                     │
           ┌─────────┴──┐
           │  MQ_MONITOR│
           │(hijo→monitor│
           └────────────┘
                     │
           ┌─────────┴──┐
           │ Pipe anónimo│
           │(padre→hijo) │
           │ (BLOQUEO)   │
           └────────────┘
```

---

## 3. Funciones Clave para la Defensa

### 3.1 `lanzar_usuario()` — banco.c — **La Función Central de la Fase II**

```c
static pid_t lanzar_usuario(int conn_fd, int *pipe_wr_out) {
    int pfd[2];
    pipe(pfd);        // Crea un pipe anónimo unidireccional
    pid_t pid = fork(); // Clona el proceso banco entero

    if (pid == 0) {   // Estamos en el HIJO
        close(pfd[1]);
        close(g_server_fd); // El hijo NO necesita el socket servidor

        dup2(conn_fd, STDIN_FILENO);   // fd 0 → socket TCP
        dup2(conn_fd, STDOUT_FILENO);  // fd 1 → socket TCP
        close(conn_fd);

        char s_fd[16];
        snprintf(s_fd, sizeof(s_fd), "%d", pfd[0]);
        char *args[] = {"usuario", s_fd, NULL};
        execv("./usuario", args); // Reemplaza la memoria con usuario.c
    }
    // PADRE: cierra lo que no necesita y devuelve el pipe de escritura
    close(conn_fd);
    close(pfd[0]);
    *pipe_wr_out = pfd[1];
    return pid;
}
```

**¿Cómo explicarla en la defensa?**

> "Cuando llega una nueva conexión TCP, el banco llama a `lanzar_usuario`. Esta función hace tres cosas muy importantes. Primero, crea un **pipe anónimo** — una tubería unidireccional en memoria — que más tarde el padre usará para enviar la orden de bloqueo al hijo si es necesario. Segundo, llama a `fork()`, que clona el proceso completo del banco. Tercero, en el proceso hijo, ejecuta dos llamadas `dup2()`: esto redirige el descriptor de STDIN (fd 0) y STDOUT (fd 1) a apuntar al socket TCP del cliente. La magia de UNIX es que `./usuario` no sabe que está en red. Sigue usando `printf` y `fgets` normalmente, pero el sistema operativo redirige esas llamadas al socket TCP, y los bytes viajan por el cable hasta el cliente Telnet. Al final, el hijo llama a `execv` que destruye la memoria del clon y carga el código de `usuario.c`."

---

### 3.2 `dup2()` — La Clave del "Engaño" al Proceso Hijo

```c
dup2(conn_fd, STDIN_FILENO);   // fd 0 ahora apunta al socket
dup2(conn_fd, STDOUT_FILENO);  // fd 1 ahora apunta al socket
```

**¿Cómo explicarlo?**

> "En UNIX, todo es un descriptor de fichero. `STDIN_FILENO` es siempre el número 0, y `STDOUT_FILENO` es siempre el número 1. La llamada `dup2(conn_fd, 0)` le dice al kernel: 'cierra el fd 0 actual (el teclado) y haz que el fd 0 apunte a donde apunta `conn_fd` (el socket TCP)'. A partir de ese momento, cualquier `fgets(stdin)` lee bytes del cable de red, y cualquier `printf` envía bytes por el cable de red. El proceso `usuario.c` no tuvo que cambiar su lógica en absoluto para funcionar en red."

---

### 3.3 El Zombie Harvester — `waitpid(-1, WNOHANG)` — banco.c

```c
while (!g_salir) {
    // --- Zombie Harvester al inicio de cada iteración ---
    int wst;
    pid_t wpid;
    while ((wpid = waitpid(-1, &wst, WNOHANG)) > 0) {
        for (int i = 0; i < g_num_hijos; i++) {
            if (g_hijos[i].pid == wpid) {
                if (g_hijos[i].pipe_wr != -1)
                    close(g_hijos[i].pipe_wr);
                g_hijos[i] = g_hijos[--g_num_hijos]; // Compacta el array
                break;
            }
        }
    }
    int ret = poll(pfds, 3, 500); // Espera hasta 500ms
    // ...
}
```

**¿Cómo explicarlo?**

> "Cuando un proceso hijo termina en Linux, no desaparece completamente. Su entrada queda en la tabla de procesos del kernel en estado 'defunct' o 'zombie', esperando que el padre recoja su código de salida con `waitpid`. Si el padre nunca lo hace, acumula zombis que consumen recursos del sistema. Para evitarlo, al inicio de cada iteración del bucle principal llamamos a `waitpid(-1, &wst, WNOHANG)`. El `-1` significa 'esperar a cualquier hijo', y `WNOHANG` significa 'no bloquearse si ninguno ha terminado'. El bucle `while` lo repite hasta que no quede ningún hijo pendiente de cosechar. Además, cuando detectamos que un hijo terminó, compactamos el array `g_hijos` para no desperdiciar memoria."

---

### 3.4 `procesar_log()` con interceptación de `OP_LOGIN` — banco.c

```c
static void procesar_log(void) {
    DatosLog dl;
    while (mq_receive(g_mq_log, (char *)&dl, sizeof(dl), NULL) > 0) {

        // Interceptar OP_LOGIN: mapear PID → cuenta_id
        if (dl.tipo_op == OP_LOGIN) {
            for (int i = 0; i < g_num_hijos; i++) {
                if (g_hijos[i].pid == dl.pid_hijo) {
                    g_hijos[i].cuenta_id = dl.cuenta_id;
                    printf("[BANCO] Hijo PID %d autenticado como cuenta %d\n",
                           dl.pid_hijo, dl.cuenta_id);
                    break;
                }
            }
            continue; // NO escribir en transacciones.log
        }
        // ... resto de tipos de operación ...
    }
}
```

**¿Por qué existe este mecanismo?**

> "Este es el Fix #2 que resuelve un problema arquitectónico crítico. En la Fase I, el banco sabía de antemano qué cuenta gestionaba cada hijo, porque era él quien lo lanzaba con el ID ya fijado. En la Fase II, el hijo llega sin cuenta asignada: primero el cliente se conecta por Telnet, y solo *después* se identifica. El banco registra cada nuevo hijo con `cuenta_id = 0` porque todavía no sabe a quién pertenece. Cuando el hijo se autentica, envía por la cola de mensajes `MQ_LOG` un mensaje especial de tipo `OP_LOGIN` con su propio PID y el ID de cuenta que eligió. El padre lo intercepta en `procesar_log`, busca en el array `g_hijos` el proceso con ese PID y actualiza su `cuenta_id`. Gracias a este mecanismo, si el monitor detecta fraude en la cuenta 1001, el padre puede buscar en `g_hijos` quién tiene `cuenta_id=1001`, encontrar su `pipe_wr`, y enviarle la orden de bloqueo por el pipe exclusivo de ese cliente."

---

### 3.5 `autenticar_usuario()` + `OP_LOGIN` — usuario.c

```c
// En main() de usuario.c, justo tras autenticarse:
g_cuenta_id = autenticar_usuario();

// Enviar OP_LOGIN al padre
DatosLog dl;
memset(&dl, 0, sizeof(dl));
dl.cuenta_id = g_cuenta_id;
dl.pid_hijo  = getpid();
dl.tipo_op   = OP_LOGIN;
dl.estado    = 1;
timestamp_ahora(dl.timestamp, sizeof(dl.timestamp));
mq_send(g_mq_log, (const char *)&dl, sizeof(dl), 0);
```

**¿Cómo explicarlo?**

> "El proceso hijo arranca sin saber quién es el usuario porque, al contrario que en la Fase I, el banco ya no le pasa el ID como argumento. Primero llama a `autenticar_usuario()`, que muestra un formulario de login por la terminal (que en realidad es el socket TCP). Una vez que el usuario introduce un número de cuenta válido y se verifica contra el fichero binario, el hijo sabe su identidad. Inmediatamente construye un mensaje `DatosLog` con su PID y su `cuenta_id`, marca el `tipo_op` como `OP_LOGIN` y lo envía por la cola `MQ_LOG` al padre. Este es el 'apretón de manos' (handshake) entre el proceso hijo y el proceso padre."

---

### 3.6 `limpiar_telnet()` — usuario.c — El Detalle del Protocolo

```c
static void limpiar_telnet(char *buf) {
    char *p = buf;
    while (*p) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        p++;
    }
}
```

**¿Cómo explicarlo?**

> "El protocolo Telnet es de los años 70 y cuando el usuario pulsa Enter, envía `\r\n` (retorno de carro + nueva línea), mientras que Linux solo espera `\n`. Si dejamos que `fgets` recoja ese buffer tal cual, el string '1001' en realidad es '1001\r', y `atoi('1001\r')` devuelve 1001 por suerte, pero hay funciones más estrictas que fallarían. La función `limpiar_telnet` simplemente recorre el buffer y coloca un terminador nulo en cuanto encuentra un `\r` o `\n`, garantizando que el string queda limpio independientemente del protocolo del cliente."

---

### 3.7 `buscar_cuenta()` — Acceso Directo O(1) al Fichero Binario

```c
int buscar_cuenta(int numero, Cuenta *c) {
    long offset = (long)(numero - ID_INICIAL) * (long)sizeof(Cuenta);
    FILE *f = fopen(g_cfg.archivo_cuentas, "rb");
    fseek(f, offset, SEEK_SET);
    int ok = (fread(c, sizeof(Cuenta), 1, f) == 1) ? 1 : 0;
    fclose(f);
    if (ok && c->numero_cuenta != numero) return 0;
    return ok;
}
```

**¿Cómo explicarlo?**

> "Las cuentas no se guardan en texto plano ni en una base de datos. Se guardan en un fichero binario donde cada cuenta ocupa exactamente `sizeof(Cuenta)` bytes. La cuenta 1001 está en el byte 0, la cuenta 1002 en el byte `sizeof(Cuenta)`, la 1003 en `2 * sizeof(Cuenta)`, y así sucesivamente. Para leer la cuenta 1003, calculamos el offset: `(1003 - 1001) * sizeof(Cuenta) = 2 * sizeof(Cuenta)`. Llamamos a `fseek` para saltar directamente a esa posición y `fread` para leer solo esos bytes. Esto es acceso directo O(1): independientemente de si hay 5 o 5 millones de cuentas, el tiempo de acceso es siempre el mismo."

---

### 3.8 `procesar_alertas()` y el mecanismo de bloqueo — banco.c

```c
static void procesar_alertas(void) {
    DatosAlerta da;
    while (mq_receive(g_mq_alerta, (char *)&da, sizeof(da), NULL) > 0) {
        int bloquear = (strstr(da.mensaje, ALERTA_RETIROS) != NULL ||
                        strstr(da.mensaje, ALERTA_TRANSFERENCIAS) != NULL);
        if (bloquear) {
            InfoHijo *h = buscar_hijo(da.cuenta_id);
            if (h && h->pipe_wr != -1) {
                char aviso[512];
                snprintf(aviso, sizeof(aviso), "BLOQUEO: %s en cuenta %d",
                         da.mensaje, da.cuenta_id);
                write(h->pipe_wr, aviso, strlen(aviso) + 1);
                close(h->pipe_wr);
                h->pipe_wr = -1;
            }
        }
    }
}
```

**¿Cómo explicarlo?**

> "El monitor detecta comportamientos sospechosos y envía una alerta por `MQ_ALERTA`. El banco la recibe en `procesar_alertas`. Si la alerta implica bloqueo (retiros excesivos o transferencias repetitivas), el banco busca en `g_hijos` el proceso responsable usando `buscar_hijo(da.cuenta_id)`. Gracias al mecanismo de `OP_LOGIN` ya sabemos exactamente qué PID gestiona la cuenta afectada. Una vez encontrado, escribe el mensaje 'BLOQUEO...' por el `pipe_wr` — el extremo de escritura del pipe anónimo que creamos en `lanzar_usuario`. En el hijo, `poll()` detecta que hay datos en su extremo de lectura (`pipe_rd`), lee el mensaje, lo muestra al usuario y llama a `exit(0)`. La conexión Telnet se cierra. Ningún otro cliente se ve afectado."

---

### 3.9 `poll()` — El Multiplexor de Eventos

#### En banco.c (Servidor):
```c
struct pollfd pfds[3];
pfds[0].fd = g_server_fd;  // Socket servidor: nuevas conexiones
pfds[1].fd = g_mq_log;     // Cola de log de los hijos
pfds[2].fd = g_mq_alerta;  // Cola de alertas del monitor

int ret = poll(pfds, 3, 500); // Esperar hasta 500ms
```

#### En usuario.c (Cliente):
```c
struct pollfd fds[2];
fds[0].fd = STDIN_FILENO;  // Entrada del teclado/red
fds[1].fd = g_pipe_rd;     // Pipe de bloqueo del padre

int ret = poll(fds, 2, -1); // Esperar indefinidamente
```

**¿Cómo explicarlo?**

> "`poll()` es la solución al problema de 'esperar a varios eventos a la vez sin bloquear'. Si el servidor hiciera un `accept()` bloqueante, no podría leer mensajes de las colas de log mientras espera conexiones. Si hiciera un `mq_receive()` bloqueante, no podría aceptar conexiones. `poll()` recibe una lista de descriptores de fichero y un timeout, y bloquea el proceso hasta que *alguno* de ellos tenga datos disponibles. Cuando `poll` retorna, comprobamos `revents` de cada descriptor para saber cuál fue el que despertó el proceso y actuamos en consecuencia. Con `timeout=500ms` en el servidor, el bucle se despierta periódicamente aunque no lleguen eventos, lo que permite ejecutar el Zombie Harvester de forma regular."

---

### 3.10 Semáforos POSIX — La Exclusión Mutua

```c
// Inicialización (solo el banco padre):
sem_unlink(SEM_CUENTAS);
sem_t *sem_c = sem_open(SEM_CUENTAS, O_CREAT | O_EXCL, 0600, 1);

// Uso en cualquier proceso:
sem_t *s = sem_open(SEM_CUENTAS, 0); // Abrir semáforo existente
sem_wait(s);   // Bajar: si vale 0, bloquear; si vale 1, pasar y poner a 0
// ... sección crítica: lectura/escritura en cuentas.dat ...
sem_post(s);   // Subir: incrementa a 1, desbloquea a quien espere
sem_close(s);  // Cerrar el handle local (no destruye el semáforo)
```

**¿Cómo explicarlo?**

> "Tenemos varios procesos `usuario` ejecutándose simultáneamente, y todos necesitan leer y escribir en el mismo fichero `cuentas.dat`. Si dos procesos hacen `fseek + fwrite` al mismo tiempo sobre la misma cuenta, pueden corromperse mutuamente los datos. Los semáforos POSIX resuelven esto. Son objetos del kernel que actúan como candados. Tienen un valor entero. `sem_wait` intenta decrementarlo: si está en 1, lo pone a 0 y el proceso sigue. Si ya está en 0, el proceso se bloquea hasta que alguien lo incremente. `sem_post` lo incrementa y desbloquea a cualquier proceso que esté esperando. Usamos dos semáforos: `SEM_CUENTAS` para proteger las escrituras en `cuentas.dat`, y `SEM_CONFIG` para proteger las actualizaciones de `PROXIMO_ID` en `config.txt`."

---

## 4. Flujo Completo de una Sesión (para explicar de memoria)

```
1. [Mac] docker run --name servidor_banco ... securebank
2. [Container] make clean && make && ./init_cuentas && ./banco
   → banco crea semáforos, colas MQ, lanza monitor, abre socket en :5000
   → poll() esperando...

3. [Mac, Terminal2] docker exec -it servidor_banco bash
4. [Container2] telnet 127.0.0.1 5000
   → accept() en banco → conn_fd
   → lanzar_usuario(conn_fd): fork() + dup2() + execv("./usuario")
   → hijo: STDIN/STDOUT → socket TCP
   → banco: guarda (pid, cuenta_id=0, pipe_wr) en g_hijos[]

5. [Telnet] El usuario escribe "1001"
   → autenticar_usuario() en hijo: fgets→limpiar_telnet→leer_cuenta(1001)→OK
   → hijo envía OP_LOGIN{pid=X, cuenta_id=1001} por MQ_LOG
   → banco intercepta: g_hijos[i].cuenta_id = 1001

6. [Telnet] El usuario hace 3 retiros
   → cada retiro: thread_retiro → mq_send(MQ_MONITOR) + mq_send(MQ_LOG)
   → monitor: detecta RETIROS_EXCESIVOS → mq_send(MQ_ALERTA)
   → banco: procesar_alertas → buscar_hijo(1001) → write(pipe_wr, "BLOQUEO...")
   → hijo: poll detecta pipe_rd → printf("ALERTA DEL BANCO") → exit(0)
   → conexión Telnet se cierra

7. [Mac, Terminal3] Un segundo cliente en cuenta 1002: SIGUE FUNCIONANDO
   (prueba que el bloqueo fue quirúrgico, solo afectó al pipe correcto)

8. [Container] Ctrl+C en banco
   → manejador_sigterm: g_salir=1
   → cierre limpio: kill todos los hijos, waitpid, mq_unlink, sem_unlink
```

---

## 5. Cambios Implementados — Resumen por Archivo

### `banco_comun.h`
- Añadidos headers de red: `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`
- Añadido `#define OP_LOGIN 5` — nuevo tipo de operación para el handshake de autenticación
- Añadido `int puerto` a la estructura `Config`

### `config.txt`
- Añadida línea `PUERTO=5000` — puerto TCP configurable sin recompilar

### `banco.c` — Servidor TCP Concurrente
| Función | Cambio |
|---|---|
| `leer_config()` | Parsea el nuevo campo `PUERTO=` |
| `guardar_proximo_id()` | Función para persistir el ID siguiente en `config.txt` |
| `buscar_cuenta()` | Acceso directo O(1) al fichero binario de cuentas |
| `crear_cuenta()` | Creación de cuenta con semáforos para exclusión mutua |
| `procesar_log()` | Intercepta `OP_LOGIN` para mapear PID→cuenta_id. No escribe este tipo en el log. |
| `procesar_alertas()` | Sin cambios lógicos, pero ahora funciona correctamente gracias al mapeo de `OP_LOGIN` |
| `lanzar_usuario()` | Ahora recibe `conn_fd`. Añadidos `dup2()` para redirigir STDIN/STDOUT al socket. Ya no pasa `cuenta_id` como argumento. |
| `main()` | Socket TCP completo (`socket→setsockopt→bind→listen`). Poll sobre `server_fd` en vez de `STDIN_FILENO`. **Zombie Harvester** con `waitpid(-1, WNOHANG)`. Cierre limpio con `SIGTERM` a todos los hijos. |

### `usuario.c` — Cliente TCP Adaptado
| Función | Cambio |
|---|---|
| `limpiar_telnet()` | **Nueva**: elimina `\r\n` de Telnet |
| `guardar_proximo_id_usuario()` | **Nueva**: réplica para uso en procesos hijo |
| `crear_cuenta_remota()` | **Nueva**: creación de cuentas desde el hijo con semáforos |
| `autenticar_usuario()` | **Nueva**: login interactivo por Telnet (0=nueva, -1=salir) |
| `pedir_divisa()` | Convertido de `scanf` a `fgets+limpiar_telnet+sscanf` |
| `procesar_opcion()` | Todas las entradas de usuario convertidas a `fgets+limpiar_telnet` |
| `main()` | Recibe solo `argv[1]=pipe_rd`. `setvbuf(_IONBF)` para envío inmediato. Llama a `autenticar_usuario()` y luego envía `OP_LOGIN`. Bucle con `fgets` en vez de `scanf`. |

---

## 6. Preguntas Frecuentes en una Defensa

**P: ¿Por qué `fork()` y no `pthread` para manejar múltiples clientes?**
> R: Con `fork()` cada cliente tiene su propio espacio de memoria aislado. Un error en un cliente (corrupción de memoria, segfault) no puede afectar a otros clientes ni al servidor principal. Con threads, un error en uno puede tumbar todo el proceso. Además, `fork()` es el modelo clásico de UNIX para servidores concurrentes y es lo que pide la asignatura (IPC de procesos, no de hilos).

**P: ¿Qué pasa si dos clientes hacen un retiro sobre la misma cuenta al mismo tiempo?**
> R: `thread_retiro` en `usuario.c` adquiere `SEM_CUENTAS` antes de hacer el `fseek+fread+cálculo+fwrite`. Como el semáforo es binario (mutex), solo uno puede estar en esa sección crítica a la vez. El segundo espera en `sem_wait` hasta que el primero haga `sem_post`.

**P: ¿Por qué `O_NONBLOCK` en las Message Queues del banco?**
> R: El banco hace `mq_receive` dentro de `procesar_log()` en un bucle `while`. Si las colas fueran bloqueantes, el banco se quedaría esperando indefinidamente si no hay mensajes, sin poder atender nuevas conexiones. Con `O_NONBLOCK`, `mq_receive` devuelve inmediatamente `-1` con `errno=EAGAIN` cuando la cola está vacía, permitiendo que el bucle termine y el control vuelva a `poll()`.

**P: ¿Cómo evitamos el "Address already in use" al reiniciar el servidor?**
> R: Con `setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))`. Cuando un servidor cierra su socket, el kernel mantiene el puerto en estado `TIME_WAIT` durante ~60 segundos para absorber paquetes retrasados de la sesión anterior. `SO_REUSEADDR` le dice al kernel que permita reutilizar ese puerto inmediatamente, lo que es esencial durante el desarrollo cuando se reinicia el servidor frecuentemente.
