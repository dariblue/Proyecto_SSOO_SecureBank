# Memoria Técnica: SecureBank
## Sistema Bancario Concurrente Avanzado en Entornos POSIX

**Grupo de Trabajo: Grupo Pizza**
**Integrantes:**
*   Eugenia Rodriguez Andrade
*   Vito Perilo Ávila
*   Dario Rodriguez Pajares

---

## 1. Introducción y Resumen Ejecutivo
SecureBank es una solución de software desarrollada en lenguaje C diseñada para simular la operación de una entidad bancaria multicliente bajo condiciones de alta concurrencia. El sistema garantiza la integridad de los datos financieros mediante mecanismos robustos de sincronización y ofrece una capa de seguridad activa capaz de detectar y mitigar intentos de fraude en tiempo real.

La arquitectura se fundamenta en los estándares **POSIX**, utilizando una jerarquía de procesos distribuidos, hilos de ejecución ligeros (`pthreads`) y comunicaciones asíncronas mediante colas de mensajes del sistema.

---

## 2. Arquitectura del Sistema: Modelo de Procesos
El sistema se estructura en tres capas de procesos independientes que interactúan entre sí para formar un ecosistema coherente:

### 2.1. El Orquestador Central (`banco.c`)
Actúa como el proceso raíz del sistema. Sus funciones críticas incluyen:
*   **Inicialización de Recursos**: Configuración de semáforos nombrados y colas de mensajes.
*   **Gestión de Descendencia**: Utiliza la primitiva `fork()` para crear procesos especializados. Tras la ramificación, utiliza `execv()` para transformar la imagen del proceso hijo en el ejecutable `monitor` o `usuario`, garantizando un aislamiento total de memoria y una carga limpia del vector de argumentos.
*   **Monitorización de Log**: Reacciona ante eventos reportados por los clientes y el vigilante para mantener una persistencia histórica de las transacciones.

### 2.2. Cliente Multihilo (`usuario.c`)
Representa la sesión activa de un cliente. 
*   **Concurrencia Híbrida**: Mientras el proceso principal gestiona la interfaz de usuario (menú) y la escucha de señales de control del padre, cada operación bancaria (depósito, retiro, transferencia o cambio de divisa) se delega a un hilo `pthread` independiente.
*   **Integridad Transaccional**: Los hilos coordinan el acceso al archivo binario de cuentas mediante semáforos, evitando colisiones de datos.

### 2.3. Vigilancia de Fraude (`monitor.c`)
Proceso dedicado al análisis heurístico de las transacciones. 
*   **Detección Reactiva**: Implementa un bucle de alta eficiencia con `poll()` para monitorizar la cola de mensajes de transacciones sin consumir CPU innecesaria.
*   **Alertas Críticas**: Al detectar patrones sospechosos (como retiros múltiples que superan el umbral configurado), notifica de inmediato al orquestador central para proceder al bloqueo.

---

## 3. Mecanismos de Comunicación e Integridad (IPC)

| Mecanismo | Uso en SecureBank | Justificación Técnica |
| :--- | :--- | :--- |
| **Colas de Mensajes (mqueue)** | `MQ_LOG`, `MQ_MONITOR`, `MQ_ALERTA` | Permite una comunicación asíncrona y estructurada entre procesos que no comparten espacio de direcciones. |
| **Semáforos Nombrados** | `/sem_cuentas`, `/sem_config` | Garantizan la exclusión mutua global a nivel de sistema, protegiendo los archivos `cuentas.dat` y `config.txt` de corrupciones por escritura concurrente. |
| **Tuberías Anónimas (Pipes)** | Comunicación Padre → Hijo | Utilizadas como un canal de control prioritario para enviar la señal de "BLOQUEO" a una sesión de usuario cuando se detecta un fraude. |

---

## 4. Implementación Avanzada y Optimizaciones

### 4.1. Acceso Aleatorio Optimizado (`fseek`)
En lugar de realizar búsquedas secuenciales —que penalizarían el rendimiento conforme la base de datos de clientes crezca—, SecureBank implementa un **acceso directo O(1)**. 
Utilizando la constante `ID_INICIAL (1001)`, se calcula el desplazamiento exacto (offset) en el archivo binario:
> `offset = (cuenta_id - 1001) * sizeof(Cuenta)`
Esto permite posicionar el puntero de lectura/escritura de forma instantánea, independientemente de la posición del registro.

### 4.2. Robustez y Cero Warnings
El código ha sido refactorizado para cumplir con el estándar `-Wall -Wextra`. Se realiza un manejo exhaustivo de los valores de retorno de funciones del sistema (`fread`, `fwrite`, `mq_send`), garantizando que cualquier fallo en la infraestructura IPC sea detectado y gestionado sin provocar el colapso del proceso.

---

## 5. Conclusión
La implementación de SecureBank demuestra una aplicación rigurosa de los conceptos de sistemas operativos. La combinación de procesos pesados para el aislamiento, hilos para la concurrencia y mecanismos POSIX para la comunicación, resulta en un sistema bancario seguro, eficiente y escalable.
