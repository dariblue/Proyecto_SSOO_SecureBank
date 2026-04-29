#include "banco_comun.h"
#include <sys/stat.h>

/* Variables globales */
static mqd_t g_mq_log = -1;
static mqd_t g_mq_alerta = -1;
static mqd_t g_mq_monitor = -1;

static pid_t g_monitor_pid = -1;
static int   g_server_fd   = -1; /* <- Fase II: descriptor del socket servidor TCP */

#define MAX_HIJOS 64
typedef struct {
  pid_t pid;
  int cuenta_id;
  int pipe_wr;
} InfoHijo;
static InfoHijo g_hijos[MAX_HIJOS];
static int g_num_hijos = 0;

static Config g_cfg;
static volatile sig_atomic_t g_salir = 0; /* <- volatile: Evita que el compilador optimice la variable. sig_atomic_t: Garantiza modificacion segura por senales OS */

/* Señales */
static void manejador_sigterm(int s) {
  (void)s;
  g_salir = 1;
}

/* Leer config.txt  */
static int leer_config(const char *ruta, Config *cfg) {
  FILE *f = fopen(ruta, "r");
  if (!f) {
    perror("fopen config");
    return -1;
  }

  memset(cfg, 0, sizeof(Config));
  char linea[256];

  while (fgets(linea, sizeof(linea), f)) {
    /* Saltar comentarios y líneas vacías */
    if (linea[0] == '#' || linea[0] == '\n' || strlen(linea) < 3)
      continue;

    if (strstr(linea, "PROXIMO_ID="))
      sscanf(linea, "PROXIMO_ID=%d", &cfg->proximo_id);
    else if (strstr(linea, "LIM_RET_EUR="))
      sscanf(linea, "LIM_RET_EUR=%f", &cfg->lim_ret_eur);
    else if (strstr(linea, "LIM_RET_USD="))
      sscanf(linea, "LIM_RET_USD=%f", &cfg->lim_ret_usd);
    else if (strstr(linea, "LIM_RET_GBP="))
      sscanf(linea, "LIM_RET_GBP=%f", &cfg->lim_ret_gbp);
    else if (strstr(linea, "LIM_TRF_EUR="))
      sscanf(linea, "LIM_TRF_EUR=%f", &cfg->lim_trf_eur);
    else if (strstr(linea, "LIM_TRF_USD="))
      sscanf(linea, "LIM_TRF_USD=%f", &cfg->lim_trf_usd);
    else if (strstr(linea, "LIM_TRF_GBP="))
      sscanf(linea, "LIM_TRF_GBP=%f", &cfg->lim_trf_gbp);
    else if (strstr(linea, "UMBRAL_RETIROS="))
      sscanf(linea, "UMBRAL_RETIROS=%d", &cfg->umbral_retiros);
    else if (strstr(linea, "UMBRAL_TRANSFERENCIAS="))
      sscanf(linea, "UMBRAL_TRANSFERENCIAS=%d", &cfg->umbral_transferencias);
    else if (strstr(linea, "NUM_HILOS="))
      sscanf(linea, "NUM_HILOS=%d", &cfg->num_hilos);
    else if (strstr(linea, "ARCHIVO_CUENTAS="))
      sscanf(linea, "ARCHIVO_CUENTAS=%255s", cfg->archivo_cuentas);
    else if (strstr(linea, "ARCHIVO_LOG="))
      sscanf(linea, "ARCHIVO_LOG=%255s", cfg->archivo_log);
    else if (strstr(linea, "CAMBIO_USD="))
      sscanf(linea, "CAMBIO_USD=%f", &cfg->cambio_usd);
    else if (strstr(linea, "CAMBIO_GBP="))
      sscanf(linea, "CAMBIO_GBP=%f", &cfg->cambio_gbp);
    else if (strstr(linea, "PUERTO="))
      sscanf(linea, "PUERTO=%d", &cfg->puerto);
  }

  fclose(f);

  printf("[BANCO] Config cargada: PROXIMO_ID=%d, HILOS=%d, CUENTAS=%s\n",
         cfg->proximo_id, cfg->num_hilos, cfg->archivo_cuentas);
  printf("[BANCO] Limites retiro  EUR=%.0f USD=%.0f GBP=%.0f\n",
         cfg->lim_ret_eur, cfg->lim_ret_usd, cfg->lim_ret_gbp);
  printf("[BANCO] Limites transf  EUR=%.0f USD=%.0f GBP=%.0f\n",
         cfg->lim_trf_eur, cfg->lim_trf_usd, cfg->lim_trf_gbp);
  printf("[BANCO] Cambio USD=%.2f  GBP=%.2f\n", cfg->cambio_usd,
         cfg->cambio_gbp);
  printf("[BANCO] Puerto TCP=%d\n", cfg->puerto);
  return 0;
}
/* Actualizar PROXIMO_ID en config.txt */
void guardar_proximo_id(int nuevo_id) {
  FILE *f = fopen("config.txt", "r");
  if (!f)
    return;
  char contenido[4096];
  size_t n = fread(contenido, 1, sizeof(contenido) - 1, f);
  contenido[n] = '\0';
  fclose(f);

  char *p = strstr(contenido, "PROXIMO_ID=");
  if (!p)
    return;
  size_t antes = (size_t)(p - contenido);
  char *fin_linea = strchr(p, '\n');

  char nuevo[4096];
  snprintf(nuevo, sizeof(nuevo), "%.*sPROXIMO_ID=%d%s", (int)antes, contenido,
           nuevo_id, fin_linea ? fin_linea : "");

  f = fopen("config.txt", "w");
  if (!f)
    return;
  fputs(nuevo, f);
  fclose(f);
}

/* Forward declaration */
static void escribir_log(const char *linea);

/* Buscar cuenta usando fseek (Acceso Directo) */
int buscar_cuenta(int numero, Cuenta *c) {
  if (numero < ID_INICIAL)
    return 0;
  long offset = (long)(numero - ID_INICIAL) * (long)sizeof(Cuenta);

  FILE *f = fopen(g_cfg.archivo_cuentas, "rb");
  if (!f)
    return 0;

  if (fseek(f, offset, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }

  int ok = (fread(c, sizeof(Cuenta), 1, f) == 1) ? 1 : 0;
  fclose(f);

  /* Validar que el registro contiene el ID correcto */
  if (ok && c->numero_cuenta != numero)
    return 0;

  return ok;
}

/* Crear nueva cuenta */
int crear_cuenta(Cuenta *nueva) {
  /* 1. Obtener y reservar el próximo ID bajo SEM_CONFIG */
  sem_t *sc = sem_open(SEM_CONFIG, 0);
  if (sc == SEM_FAILED) {
    perror("sem_open SEM_CONFIG");
    return -1;
  }

  sem_wait(sc);
  nueva->numero_cuenta = g_cfg.proximo_id;
  g_cfg.proximo_id++;
  guardar_proximo_id(g_cfg.proximo_id);
  sem_post(sc);
  sem_close(sc);

  /* 2. Escribir la nueva cuenta en el offset exacto según su ID */
  sem_t *sa = sem_open(SEM_CUENTAS, 0); /* <- Abre y mapea el descriptor del candado global POSIX ya inicializado */
  if (sa == SEM_FAILED) {
    perror("sem_open SEM_CUENTAS");
    return -1;
  }

  sem_wait(sa);
  /* Usar "rb+" para permitir fseek y escritura en posición media/final sin
   * truncar */
  FILE *f = fopen(g_cfg.archivo_cuentas, "rb+");
  if (!f) {
    /* Si no existe, lo creamos de cero */
    f = fopen(g_cfg.archivo_cuentas, "wb+");
  }

  if (f) {
    long offset =
        (long)(nueva->numero_cuenta - ID_INICIAL) * (long)sizeof(Cuenta);
    fseek(f, offset, SEEK_SET); /* <- Mueve instantaneamente (O(1)) el puntero de disco a los bytes de la cuenta exacta */
    fwrite(nueva, sizeof(Cuenta), 1, f);
    fflush(f);
    fclose(f);
  }
  sem_post(sa);
  sem_close(sa);

  /* 3. Registrar en el log */
  char ts[MAX_TS];
  timestamp_ahora(ts, sizeof(ts));
  char linea[512];
  snprintf(linea, sizeof(linea), "[%s] NUEVA CUENTA: id=%d titular=%s", ts,
           nueva->numero_cuenta, nueva->titular);
  escribir_log(linea);

  printf("Cuenta %d creada para '%s'\n", nueva->numero_cuenta, nueva->titular);
  return nueva->numero_cuenta;
}

/* Forward declaration */
static void escribir_log(const char *linea);

/* Escribir en el log */
static void escribir_log(const char *linea) {
  FILE *f = fopen(g_cfg.archivo_log, "a");
  if (!f) {
    perror("fopen log");
    return;
  }
  fprintf(f, "%s\n", linea);
  fflush(f);
  fclose(f);
}

/* Buscar hijo por cuenta_id */
static InfoHijo *buscar_hijo(int cuenta_id) {
  for (int i = 0; i < g_num_hijos; i++)
    if (g_hijos[i].cuenta_id == cuenta_id)
      return &g_hijos[i];
  return NULL;
}

/* Procesar mensajes de log pendientes en MQ_LOG */
static void procesar_log(void) {
  DatosLog dl;
  while (mq_receive(g_mq_log, (char *)&dl, sizeof(dl), NULL) > 0) {

    /* Fase II: Interceptar OP_LOGIN para mapear PID -> cuenta_id.
     * Cuando un hijo se autentica por Telnet, envia este mensaje
     * para que el padre sepa a que cuenta pertenece cada PID,
     * permitiendo que el mecanismo de bloqueo por pipe funcione. */
    if (dl.tipo_op == OP_LOGIN) {
      for (int i = 0; i < g_num_hijos; i++) {
        if (g_hijos[i].pid == dl.pid_hijo) {
          g_hijos[i].cuenta_id = dl.cuenta_id;
          printf("[BANCO] Hijo PID %d autenticado como cuenta %d\n",
                 dl.pid_hijo, dl.cuenta_id);
          break;
        }
      }
      continue; /* No escribir OP_LOGIN en transacciones.log */
    }

    const char *op_str;
    switch (dl.tipo_op) {
    case OP_DEPOSITO:
      op_str = "DEPOSITO";
      break;
    case OP_RETIRO:
      op_str = "RETIRO";
      break;
    case OP_TRANSFERENCIA:
      op_str = "TRANSFERENCIA";
      break;
    case OP_MOVER_DIVISA:
      op_str = "MOVER_DIVISA";
      break;
    default:
      op_str = "DESCONOCIDO";
      break;
    }
    char linea[512];
    snprintf(linea, sizeof(linea),
             "[%s] %s cuenta=%d monto=%.2f divisa=%s estado=%s pid=%d",
             dl.timestamp, op_str, dl.cuenta_id, dl.cantidad,
             nombre_divisa(dl.divisa), dl.estado ? "OK" : "FALLO", dl.pid_hijo);
    escribir_log(linea);
  }
}

/* Procesar alertas pendientes en MQ_ALERTA */
static void procesar_alertas(void) {
  DatosAlerta da;
  while (mq_receive(g_mq_alerta, (char *)&da, sizeof(da), NULL) > 0) {
    char ts[MAX_TS];
    timestamp_ahora(ts, sizeof(ts));
    char linea[512];
    int bloquear = (strstr(da.mensaje, ALERTA_RETIROS) != NULL ||
                    strstr(da.mensaje, ALERTA_TRANSFERENCIAS) != NULL);
    snprintf(linea, sizeof(linea), "[%s] ALERTA: %s - cuenta %s", ts,
             da.mensaje, bloquear ? "BLOQUEADA" : "monitoreada");
    escribir_log(linea);

    if (bloquear) {
      InfoHijo *h = buscar_hijo(da.cuenta_id);
      if (h && h->pipe_wr != -1) {
        char aviso[512];
        snprintf(aviso, sizeof(aviso), "BLOQUEO: %s en cuenta %d", da.mensaje,
                 da.cuenta_id);
        write(h->pipe_wr, aviso, strlen(aviso) + 1);
        close(h->pipe_wr);
        h->pipe_wr = -1;
      }
    }
  }
}

/* Lanzar proceso usuario (Fase II: recibe conn_fd del socket TCP)
 * Ya NO recibe cuenta_id: el hijo se autenticara solo y nos lo
 * comunicara via OP_LOGIN por la cola MQ_LOG. */
static pid_t lanzar_usuario(int conn_fd, int *pipe_wr_out) {
  int pfd[2];
  if (pipe(pfd) < 0) /* <- Crea tubería anonima Unidireccional: pfd[1] para escribir, pfd[0] para leer */ {
    perror("pipe");
    return -1;
  }

  pid_t pid = fork(); /* <- Clona íntegramente el proceso (banco) en un nuevo Hijo idéntico */
  if (pid < 0) {
    perror("fork");
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }

  if (pid == 0) {
    /* ════════════════════════════════════════════════════════════
     * HIJO: Preparar entorno de red antes de cargar ./usuario
     * ════════════════════════════════════════════════════════════ */
    close(pfd[1]);        /* Cierra extremo escritura del pipe (solo lee) */
    close(g_server_fd);   /* El hijo NO necesita el socket de escucha */

    /* Redirige STDIN y STDOUT al socket de red (conn_fd).
     * Despues de esto, cualquier printf() del proceso hijo
     * sale por el cable TCP hacia el cliente Telnet,
     * y cualquier scanf()/fgets() lee lo que el usuario teclea.
     *
     * dup2(conn_fd, STDIN_FILENO):
     *   - Cierra el STDIN original (fd 0)
     *   - Copia conn_fd en la posicion 0
     *   - Ahora fd 0 apunta al socket TCP
     *
     * dup2(conn_fd, STDOUT_FILENO):
     *   - Cierra el STDOUT original (fd 1)
     *   - Copia conn_fd en la posicion 1
     *   - Ahora fd 1 apunta al socket TCP
     *
     * El programa ./usuario NO sabe que esta en red.
     * Sigue usando printf/scanf como si fuera terminal local.
     * Es la belleza de UNIX: "todo es un descriptor de fichero". */
    dup2(conn_fd, STDIN_FILENO);
    dup2(conn_fd, STDOUT_FILENO);
    close(conn_fd);       /* Ya no necesitamos el fd original, 0 y 1 apuntan al socket */

    char s_fd[16];
    snprintf(s_fd, sizeof(s_fd), "%d", pfd[0]);
    /* Fase II: solo pasamos pipe_rd; el hijo se autentica solo */
    char *args[] = {"usuario", s_fd, NULL};
    execv("./usuario", args); /* <- Destruye la memoria del clon y carga el codigo nuevo de "usuario.c" */
    perror("execv usuario");
    _exit(1);
  }

  /* Padre: cierra descriptores que ya no necesita */
  close(conn_fd);  /* <- El padre NO necesita el socket individual del cliente */
  close(pfd[0]);   /* <- Cierra extremo lectura; el padre solo escribe (BLOQUEO) */
  *pipe_wr_out = pfd[1];
  return pid;
}

/* Lanzar proceso monitor */
static void lanzar_monitor(void) {
  pid_t pid = fork(); /* <- Clona íntegramente el proceso (banco) en un nuevo Hijo idéntico */
  if (pid < 0) {
    perror("fork monitor");
    return;
  }
  if (pid == 0) {
    char *args[] = {"monitor", NULL};
    execv("./monitor", args);
    perror("execv monitor");
    _exit(1);
  }
  g_monitor_pid = pid;
  printf("[BANCO] Monitor lanzado (PID %d)\n", pid);
}

int main(void) {
  signal(SIGTERM, manejador_sigterm);
  signal(SIGINT, manejador_sigterm);
  signal(SIGCHLD, SIG_DFL);

  if (leer_config("config.txt", &g_cfg) < 0) {
    fprintf(stderr, "No se pudo leer config.txt\n");
    return 1;
  }

  sem_unlink(SEM_CUENTAS); /* <- Unlink desregistra el semaforo del kernel (/dev/shm). Vital porque sino quedarian vivos infinitamente */
  sem_unlink(SEM_CONFIG);
  sem_t *sem_c = sem_open(SEM_CUENTAS, O_CREAT | O_EXCL, 0600, 1); /* <- O_CREAT: Crear. O_EXCL: Falla si ya existe. 0600: Permisos CHMOD. 1: Valor (Semaforo Binario / Mutex) */
  sem_t *sem_g = sem_open(SEM_CONFIG, O_CREAT | O_EXCL, 0600, 1);
  if (sem_c == SEM_FAILED || sem_g == SEM_FAILED) {
    perror("sem_open inicial");
    return 1;
  }
  sem_close(sem_c);
  sem_close(sem_g);

  int fd = open(g_cfg.archivo_cuentas, O_CREAT | O_RDWR, 0644);
  if (fd >= 0)
    close(fd);

  /* Crear las tres colas POSIX en modo no bloqueante para poder
   * drenarlas con mq_receive en un bucle sin quedarse colgado    */
  struct mq_attr attr;
  memset(&attr, 0, sizeof(attr)); /* <- Limpia toda la basura de RAM de la estructura asignandola a Ceros absolutos */
  attr.mq_maxmsg = MQ_MAXMSG;
  attr.mq_flags = O_NONBLOCK; /* <- Mails: Flag critico. Impide que las colas bloqueen el proceso entero esperandolas. */

  mq_unlink(MQ_MONITOR); /* <- Lo mismo que los semaforos pero limpiando buzones MQ en /dev/mqueue */
  mq_unlink(MQ_LOG);
  mq_unlink(MQ_ALERTA);

  attr.mq_msgsize = sizeof(DatosMonitor);
  g_mq_monitor =
      mq_open(MQ_MONITOR, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);

  attr.mq_msgsize = sizeof(DatosLog);
  g_mq_log = mq_open(MQ_LOG, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);

  attr.mq_msgsize = sizeof(DatosAlerta);
  g_mq_alerta = mq_open(MQ_ALERTA, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);

  if (g_mq_monitor == -1 || g_mq_log == -1 || g_mq_alerta == -1) {
    perror("mq_open");
    return 1;
  }

  lanzar_monitor();

  /* ═══════════════════════════════════════════════════════════
   * Fase II: Inicializacion del Socket Servidor TCP
   * ═══════════════════════════════════════════════════════════ */
  g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_server_fd < 0) { perror("socket"); return 1; }

  /* SO_REUSEADDR: Permite reutilizar el puerto inmediatamente tras cerrar
   * el servidor, evitando el error "Address already in use" */
  int opt = 1;
  setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in srv_addr;
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family      = AF_INET;
  srv_addr.sin_addr.s_addr = INADDR_ANY;       /* Escuchar en todas las interfaces */
  srv_addr.sin_port        = htons(g_cfg.puerto); /* htons: Host-TO-Network-Short (Big Endian) */

  if (bind(g_server_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
    perror("bind"); close(g_server_fd); return 1;
  }
  if (listen(g_server_fd, 5) < 0) {
    perror("listen"); close(g_server_fd); return 1;
  }

  printf("\n+======================================+\n");
  printf("|  SecureBank — Servidor TCP Fase II   |\n");
  printf("|  Escuchando en puerto %-14d |\n", g_cfg.puerto);
  printf("+======================================+\n");
  printf("[BANCO] Esperando conexiones Telnet...\n");

  /* ═══════════════════════════════════════════════════════════
   * poll() sobre socket servidor (reemplaza STDIN)
   * pfds[0] = socket servidor: detecta conexiones entrantes
   * pfds[1] = MQ_LOG:  mensajes de log de los hijos
   * pfds[2] = MQ_ALERTA: alertas del monitor
   * ═══════════════════════════════════════════════════════════ */
  struct pollfd pfds[3];
  pfds[0].fd     = g_server_fd;
  pfds[0].events = POLLIN;
  pfds[1].fd     = g_mq_log;
  pfds[1].events = POLLIN;
  pfds[2].fd     = g_mq_alerta;
  pfds[2].events = POLLIN;

  while (!g_salir) {

    /* ══ Cosechador de Zombis (Zombie Harvester) ══
     * Limpia automaticamente cualquier hijo que haya terminado su
     * sesion Telnet. Sin esto, los procesos finalizados quedarian
     * como zombis (defunct) consumiendo entradas en la tabla de
     * procesos del kernel. */
    {
      int wst;
      pid_t wpid;
      while ((wpid = waitpid(-1, &wst, WNOHANG)) > 0) {
        for (int i = 0; i < g_num_hijos; i++) {
          if (g_hijos[i].pid == wpid) {
            if (g_hijos[i].pipe_wr != -1)
              close(g_hijos[i].pipe_wr); /* Liberar pipe de bloqueo */
            printf("[BANCO] Hijo PID %d (cuenta %d) finalizado\n",
                   wpid, g_hijos[i].cuenta_id);
            g_hijos[i] = g_hijos[--g_num_hijos]; /* Desplazar ultimo sobre hueco */
            break;
          }
        }
      }
    }

    int ret = poll(pfds, 3, 500); /* Timeout 500ms: permite cosechar zombis periodicamente */
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    /* Procesar logs y alertas de las colas POSIX */
    if (pfds[1].revents & POLLIN)
      procesar_log();
    if (pfds[2].revents & POLLIN)
      procesar_alertas();

    /* ══ Conexion entrante en el socket servidor ══ */
    if (pfds[0].revents & POLLIN) {
      struct sockaddr_in cli_addr;
      socklen_t cli_len = sizeof(cli_addr);
      int conn_fd = accept(g_server_fd, (struct sockaddr *)&cli_addr, &cli_len);
      if (conn_fd < 0) { perror("accept"); continue; }

      int pipe_wr;
      pid_t pid = lanzar_usuario(conn_fd, &pipe_wr);
      /* conn_fd ya cerrado por lanzar_usuario() en el padre */
      if (pid < 0)
        continue;

      if (g_num_hijos < MAX_HIJOS) {
        g_hijos[g_num_hijos].pid       = pid;
        g_hijos[g_num_hijos].cuenta_id = 0; /* Se actualizara con OP_LOGIN */
        g_hijos[g_num_hijos].pipe_wr   = pipe_wr;
        g_num_hijos++;
      } else {
        fprintf(stderr, "[BANCO] Max hijos alcanzado, cerrando conexion\n");
        close(pipe_wr);
      }

      printf("[BANCO] Nueva conexion aceptada (PID hijo %d)\n", pid);
    }
  }

  /* ═══ Cierre limpio del servidor ═══ */
  g_salir = 1;

  /* Cerrar socket servidor para rechazar nuevas conexiones */
  if (g_server_fd >= 0)
    close(g_server_fd);

  /* Terminar todos los hijos activos */
  for (int i = 0; i < g_num_hijos; i++) {
    if (g_hijos[i].pipe_wr != -1)
      close(g_hijos[i].pipe_wr);
    kill(g_hijos[i].pid, SIGTERM);
    waitpid(g_hijos[i].pid, NULL, 0);
  }
  if (g_monitor_pid > 0) {
    kill(g_monitor_pid, SIGTERM);
    waitpid(g_monitor_pid, NULL, 0);
  }

  mq_close(g_mq_monitor);
  mq_unlink(MQ_MONITOR); /* <- Lo mismo que los semaforos pero limpiando buzones MQ en /dev/mqueue */
  mq_close(g_mq_log);
  mq_unlink(MQ_LOG);
  mq_close(g_mq_alerta);
  mq_unlink(MQ_ALERTA);
  sem_unlink(SEM_CUENTAS); /* <- Unlink desregistra el semaforo del kernel (/dev/shm). Vital porque sino quedarian vivos infinitamente */
  sem_unlink(SEM_CONFIG);

  printf("[BANCO] Servidor cerrado.\n");
  return 0;
}
