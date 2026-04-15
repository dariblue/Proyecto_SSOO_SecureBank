# 🏦 El Flujo de SecureBank: ¿Cómo funciona todo esto?

¡Hola! Vamos a desgranar cómo funciona SecureBank desde que lo arrancas hasta que haces operaciones o intentas hacer "cosillas ilegales" con la cuenta. Imagina que el sistema es un banco de verdad con su director, sus cajeros y sus cámaras de seguridad.

---

## 🏛️ 1. El Director del Banco arranca (`banco.c`)

Cuando ejecutas `./banco`, estás abriendo las puertas de la sucursal. Este es el programa principal (el "proceso padre"). Esto es lo que hace nada más abrir:

1. **Lee las reglas:** Abre `config.txt` para saber los límites de dinero, cómo se llaman los semáforos, etc.
2. **Prepara las "tuberías" y los semáforos:** Crea los buzones donde los programas se van a comunicar (las colas POSIX `MQ_LOG`, `MQ_MONITOR`, `MQ_ALERTA`) y los candados virtuales (Semáforos) para que dos personas no puedan tocar el archivo de las cuentas a la vez.
3. **Contrata al vigilante:** De forma automática, "clona" su propio proceso con una función llamada `fork()` y lo transforma en el programa `monitor` usando `execv()`. El vigilante ya está trabajando en la sombra.
4. **Se queda en el mostrador:** Muestra el menú principal (`0 = Crear cuenta`, `ID = Entrar`, `salir`). 
   * Además de estar esperando tu respuesta en el menú, el Director tiene "un ojo clínico" (usando una función llamada `poll()`) escuchando si le llegan **Logs** (para escribirlos en el disco) o **Alertas** del vigilante.

---

## 🧑‍💻 2. Entra un Cliente (`usuario.c`)

Si en el menú principal escribes un número de cuenta (por ejemplo, `1001`), el Director te atiende.

1. **Crea una sesión aislada:** El Director vuelve a usar `fork()` y `execv()` para lanzar un proceso hijo, pero esta vez ejecuta `./usuario`. 
2. **El "Cable Rojo" (Pipe):** Antes de lanzarte, el Director conecta un cable unidireccional (un *pipe* o tubería) entre él y tu sesión de usuario. Este cable sirve para una sola cosa: que el Director pueda mandarte un mensaje fulminante si te portas mal.
3. **El Cajero Automático:** Tu sesión de `./usuario` toma el control de la pantalla y te muestra tu propio sub-menú (Depósitos, Transferencias, etc.).

### 🏃‍♂️ 2.1. Haciendo una operación (Los Hilos)

Supongamos que quieres hacer un **Depósito**. En un programa básico, el menú se quedaría "congelado" mientras se guarda el archivo. Aquí no:

1. El programa `./usuario` crea un "clon menor" llamado **Hilo** (`pthread`). El menú vuelve a estar disponible inmediatamente para que te muevas por él.
2. El Hilo va directo al archivo `cuentas.dat`, pero antes... ¡coge la llave! Hace un `sem_wait()` para asegurarse de que nadie más en todo el ordenador esté modificando cuentas en ese milisegundo.
3. Usa la magia de la función `fseek()` para saltar exactamente a la línea de disco que corresponde a tu cuenta, suma el dinero, guarda y suelta la llave (`sem_post()`).
4. **Chivatazo 1:** Manda un correo al buzón `MQ_LOG` diciendo "Eh, el usuario 1001 acaba de meter 500€". El Director (`banco.c`) recibe esto y lo anota en `transacciones.log`.
5. **Chivatazo 2:** Manda otro correo ultrarrápido al buzón `MQ_MONITOR` diciendo "Movimiento del usuario 1001" para que el Vigilante lo escanee. Tras esto, el Hilo muere en paz.

---

## 🕵️ 3. El Vigilante en Tensión (`monitor.c`)

El vigilante es un programa paranoico e invisible que arrancó al principio del todo.

1. Está en un bucle infinito escuchando el buzón `MQ_MONITOR`.
2. Supongamos que haces **3 Retiros rápidos seguidos**. El cajero manda esos 3 avisos al y llegan a `MQ_MONITOR`.
3. El Vigilante lee los avisos. Tiene una lista interna donde anota tus movimientos. En el tercer retiro consecutivo dice: *"¡Límite superado! ¡Fraude!"*.
4. Manda un mensaje de S.O.S al buzón de Alertas especiales: `MQ_ALERTA`.

---

## 🚨 4. ¡BLOQUEO! (El clímax del sistema)

1. El Director (`banco.c`) que estaba tranquilo en el mostrador recibe el S.O.S del Vigilante a través de `MQ_ALERTA`.
2. Identifica que es el usuario 1001. 
3. Se acuerda del "Cable Rojo" (el *pipe*) que conectó cuando creó la sesión del usuario 1001.
4. Grita por el cable la palabra: `"BLOQUEO"`.
5. La terminal de `./usuario`, que también está escuchando ese cable de forma asíncrona (con `poll()`), escucha la palabra maldita, limpia la pantalla, te echa a patadas y destruye el proceso.
6. Automáticamente, te encuentras de nuevo en el menú principal del Director (`banco.c`).

---

## 🧼 5. Cierre y Limpieza

Cuando escribes `salir` en el menú principal:
1. El Director cierra el banco.
2. Destruye los semáforos, vacía los buzones (colas POSIX) de correo y liquida amablemente a cualquier proceso vigilante o usuario que quedara suelto por ahí.

Y ese es, esencialmente, el hermoso baile del Proyecto SecureBank. ¡Una maquinaria donde varias piezas de software hablan entre sí en milisegundos!
