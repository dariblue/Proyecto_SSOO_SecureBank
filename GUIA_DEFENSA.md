# Guion de Defensa y Plan de Pruebas: SecureBank
## Guía Estratégica - Grupo Pizza (Uso Interno)

Este documento es nuestra "hoja de ruta" para la defensa presencial ante el profesor. El objetivo es demostrar la robustez técnica del sistema de forma fluida.

---

### 1. Preparación Pre-Defensa
Antes de que el profesor se acerque a la mesa:
1.  Tener Docker Desktop funcionando.
2.  Tener una terminal abierta en la carpeta del proyecto.
3.  **Compilación limpia**: Ejecutar `make clean && make` para mostrar que no hay warnings.

---

### 2. Guion de la Demostración en Vivo

#### Paso 1: Inicialización y Creación de Cuenta
*   **Acción**: Ejecutar `./init_cuentas` y luego `./banco`.
*   **Explicación**: "Iniciamos el orquestador. El sistema ha creado las colas POSIX y los semáforos necesarios. El proceso `monitor` ya está corriendo en background mediante `execv`."
*   **Prueba**: Elegir la **Opción 0** (Crear cuenta) y crear a `John Doe`. El sistema le asignará el ID `1001`.

#### Paso 2: Operaciones y Concurrencia
*   **Acción**: Entrar con el ID `1001`. Hacer un **Depósito** de 500€.
*   **Explicación**: "Cada vez que elijo una opción, el proceso cliente lanza un hilo `pthread`. Al escribir en el archivo, el hilo adquiere el semáforo `/sem_cuentas`. Esto garantiza que, aunque otro usuario intente acceder al mismo tiempo, el archivo no se corrompa."
*   **Prueba**: Hacer una **Transferencia** a una cuenta inexistente (ver error) y luego a una existente.

#### Paso 3: Activación del Monitor (Detección de Fraude)
*   **Acción**: Realizar **3 Retiros** rápidos de 20€ cada uno.
*   **Explicación**: "Aquí estamos simulando un patrón sospechoso. El proceso `monitor` está analizando la cola `MQ_MONITOR`. Al alcanzar el umbral de 3 retiros consecutivos, el monitor disparará una alerta crítica."
*   **Resultado**: En la terminal, aparecerá el mensaje: `[ALERTA] Monitor detectó fraude en cuenta 1001`. Inmediatamente después, el proceso `banco` enviará la señal `BLOQUEO` por el pipe y la sesión del usuario se cerrará automáticamente.

#### Paso 4: Verificación de Logs
*   **Acción**: Salir del banco y mostrar el archivo de log (o el log por pantalla).
*   **Explicación**: "Cualquier operación, haya tenido éxito o no, ha sido reportada por los hijos al padre vía `MQ_LOG`. Esto asegura que el banco siempre tenga un registro de auditoría, incluso si un proceso de usuario falla."

---

### 3. Posibles Preguntas del Tribunal (Q&A)

*   **P: ¿Por qué habéis usado `fseek` en lugar de leer todo el archivo en memoria?**
    *   *R: Por escalabilidad. Si tuviéramos un millón de cuentas, leer el archivo entero sería ineficiente. Con `fseek` y el cálculo de offset, el tiempo de acceso es constante (O(1)).*
*   **P: ¿Qué pasa si el proceso `monitor` muere?**
    *   *R: El orquestador detectaría la muerte del hijo mediante `waitpid()` o una señal `SIGCHLD` (podemos indicar que esto se podría ampliar con una política de reinicio automático).*
*   **P: ¿Por qué usáis semáforos nombrados?**
    *   *R: Porque nuestros procesos (banco y usuario) no comparten memoria (no son hilos del mismo proceso). Los semáforos nombrados de POSIX son la única forma de sincronizar procesos independientes que acceden a un recurso común como el disco.*

---

**¡A por el 10, Grupo Pizza!**
