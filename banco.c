#include "banco_comun.h"
#include <sys/stat.h>

/* Variables globales */
static mqd_t g_mq_log = -1;
static mqd_t g_mq_alerta = -1;
static mqd_t g_mq_monitor = -1;

static pid_t g_monitor_pid = -1;

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
  return 0;
}

/* Actualizar PROXIMO_ID en config.txt */
static void guardar_proximo_id(int nuevo_id) {
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
static int buscar_cuenta(int numero, Cuenta *c) {
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
static int crear_cuenta(Cuenta *nueva) {
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

/* Lanzar proceso usuario */
static pid_t lanzar_usuario(int cuenta_id, int *pipe_wr_out) {
  int pfd[2];
  if (pipe(pfd) < 0) /* <- Crea tubería anonima Unidireccional: pfd[1] para escribir, pfd[0] para leer */ {
    perror("pipe");
    return -1;
  }

  pid_t pid = fork(); /* <- Clona íntegramente el proceso (banco) en un nuevo Hijo idéntico */
  if (pid < 0) {
    perror("fork");
    close(pfd[0]); /* <- Buena praxis POSIX: Como somos Padre (Banco), cerramos canal lectura y quedamos solo modo escritura */
    close(pfd[1]);
    return -1;
  }

  if (pid == 0) {
    /* Hijo: cierra extremo de escritura, usa el de lectura */
    close(pfd[1]);
    char s_id[16], s_fd[16];
    snprintf(s_id, sizeof(s_id), "%d", cuenta_id);
    snprintf(s_fd, sizeof(s_fd), "%d", pfd[0]);
    char *args[] = {"usuario", s_id, s_fd, NULL};
    execv("./usuario", args); /* <- Destruye la memoria del clon y carga el codigo nuevo de "usuario.c" a capela */
    perror("execv usuario");
    _exit(1);
  }

  /* Padre: cierra extremo de lectura, guarda el de escritura */
  close(pfd[0]); /* <- Buena praxis POSIX: Como somos Padre (Banco), cerramos canal lectura y quedamos solo modo escritura */
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

  printf("\n+==============================+\n");
  printf("|    SecureBank  --  Login     |\n");
  printf("+==============================+\n");

  /* poll sobre stdin, MQ_LOG y MQ_ALERTA directamente */
  struct pollfd pfds[3];
  pfds[0].fd = STDIN_FILENO;
  pfds[0].events = POLLIN;
  pfds[1].fd = g_mq_log;
  pfds[1].events = POLLIN;
  pfds[2].fd = g_mq_alerta;
  pfds[2].events = POLLIN;

  while (!g_salir) {

    printf("\nIntroduzca numero de cuenta (0=nueva, -1=salir): ");
    fflush(stdout);

    int ret = poll(pfds, 3, -1); /* <- Bloqueo Inteligente: Escucha Raton/Teclado Y Tuberías de log/alertas SIN comer CPU */
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (pfds[1].revents & POLLIN)
      procesar_log();
    if (pfds[2].revents & POLLIN)
      procesar_alertas();
    if (!(pfds[0].revents & POLLIN))
      continue;

    int numero;
    if (scanf("%d", &numero) != 1) {
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
        ;
      continue;
    }
    if (numero == -1) {
      g_salir = 1;
      break;
    }

    Cuenta cuenta;
    memset(&cuenta, 0, sizeof(cuenta));

    if (numero == 0) {
      printf("Nombre del titular: ");
      fflush(stdout);
      if (scanf(" %49[^\n]", cuenta.titular) != 1)
        strcpy(cuenta.titular, "Desconocido");
      cuenta.saldo_eur = cuenta.saldo_usd = cuenta.saldo_gbp = 0.0f;
      int id = crear_cuenta(&cuenta);
      if (id < 0) {
        fprintf(stderr, "Error al crear cuenta.\n");
        continue;
      }
      numero = id;
    } else {
      if (!buscar_cuenta(numero, &cuenta)) {
        printf("Cuenta %d no encontrada.\n", numero);
        continue;
      }
      printf("Bienvenido, %s (cuenta %d)\n", cuenta.titular, numero);
    }

    int pipe_wr;
    pid_t pid = lanzar_usuario(numero, &pipe_wr);
    if (pid < 0)
      continue;

    g_hijos[g_num_hijos].pid = pid;
    g_hijos[g_num_hijos].cuenta_id = numero;
    g_hijos[g_num_hijos].pipe_wr = pipe_wr;
    g_num_hijos++;

    /* Fase sesión: poll sobre las colas y waitpid mientras el hijo vive */
    struct pollfd pfd_ses[2];
    pfd_ses[0].fd = g_mq_log;
    pfd_ses[0].events = POLLIN;
    pfd_ses[1].fd = g_mq_alerta;
    pfd_ses[1].events = POLLIN;

    while (!g_salir) {
      poll(pfd_ses, 2, 200);

      if (pfd_ses[0].revents & POLLIN)
        procesar_log();
      if (pfd_ses[1].revents & POLLIN)
        procesar_alertas();

      int wst;
      if (waitpid(pid, &wst, WNOHANG) == pid) { /* <- WNOHANG: No se congela. El padre tira la caña sin esperar. Retorna pid si acabo */
        if (pipe_wr != -1) {
          close(pipe_wr);
          pipe_wr = -1;
        }
        for (int i = 0; i < g_num_hijos; i++) {
          if (g_hijos[i].pid == pid) {
            g_hijos[i] = g_hijos[--g_num_hijos];
            break;
          }
        }
        break;
      }
    }
  }

  /* Cierre */
  g_salir = 1;

  for (int i = 0; i < g_num_hijos; i++) {
    if (g_hijos[i].pipe_wr != -1)
      close(g_hijos[i].pipe_wr);
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
  return 0;
}
