# Guía Práctica — SecureBank: Cómo usar la Fase 2

Dado que estás en un Mac y el puerto `5000` suele estar bloqueado por el sistema operativo (AirPlay Receiver), la forma más sencilla de probar el banco sin problemas de conexión ni necesidad de instalar nada en tu Mac es **hacerlo todo dentro de Docker**. 

He modificado el `Dockerfile` para que incluya `telnet`, de forma que podamos usar varios clientes directamente dentro del entorno Linux virtual.

Sigue estos pasos en orden, abriendo **varias terminales** en tu Mac.

---

## TERMINAL 1: Iniciar el Servidor del Banco

Esta terminal actuará como el ordenador principal del banco.

**Paso 1: Construir la imagen del contenedor (solo hace falta una vez)**
En la carpeta del proyecto, ejecuta:
```bash
docker build -t securebank .
```

**Paso 2: Levantar el contenedor y entrar en él**
Para evitar conflictos de puertos en tu Mac, iniciaremos el contenedor asignándole un nombre específico (`servidor_banco`) sin mapear puertos hacia fuera:
```bash
docker run --name servidor_banco -it --rm -v "$(pwd):/securebank" securebank
```
*(Ahora estarás dentro del Linux virtual, verás un prompt parecido a `root@xxxxx:/securebank#`)*

**Paso 3: Compilar e inicializar datos**
Dentro del contenedor, compila el código C y crea la base de datos de prueba:
```bash
make clean && make
./init_cuentas
```

**Paso 4: Arrancar el Banco**
Inicia el servidor principal.
```bash
./banco
```
Verás un mensaje diciendo que está escuchando en el puerto `5000`. ¡Déjalo ejecutándose!

---

## TERMINAL 2: Conectar el primer Cliente Remoto

Abre una **segunda ventana/pestaña de terminal** en tu Mac. Esta simulará ser un cajero automático o un usuario en su casa.

**Paso 5: Entrar en el mismo contenedor**
Como el banco ya está corriendo en el contenedor `servidor_banco`, vamos a "infiltrarnos" en ese mismo contenedor abriendo una nueva sesión de bash:
```bash
docker exec -it servidor_banco bash
```
*(De nuevo, estarás dentro de Linux: `root@xxxxx:/securebank#`)*

**Paso 6: Conectarse usando Telnet**
Como he añadido `telnet` al contenedor, no necesitas instalarlo en tu Mac. Ejecuta:
```bash
telnet 127.0.0.1 5000
```
Verás el menú de bienvenida de SecureBank pidiéndote el número de cuenta.
*   **Para entrar con una cuenta existente**: Escribe `1001`, `1002`, `1003`, etc.
*   **Para crear una cuenta nueva remota**: Escribe `0`. Te pedirá tu nombre y te generará una cuenta nueva automáticamente.

---

## TERMINAL 3: Probar la Concurrencia (Opcional)

Si quieres ver cómo el servidor TCP maneja a varias personas a la vez (el objetivo principal de esta Fase 2), abre una **tercera ventana de terminal** en tu Mac y repite exactamente los mismos pasos de la Terminal 2:

1. `docker exec -it servidor_banco bash`
2. `telnet 127.0.0.1 5000`
3. Inicia sesión con otra cuenta (ej: `1002`).

Podrás hacer depósitos y transferencias en ambas terminales de forma simultánea. ¡El banco ya no se bloquea!

---

## 🛠️ Cómo detener todo y salir

1. En los **clientes** (Terminal 2 y 3): Simplemente escoge la opción `6. Salir` en el menú del banco. Esto cerrará la conexión Telnet. Para salir de Linux escribe `exit`.
2. En el **servidor** (Terminal 1): Pulsa `Ctrl + C` para detener el banco de forma segura. Todos los procesos hijos y semáforos se limpiarán. Para salir de Linux escribe `exit` y el contenedor se destruirá automáticamente dejándolo todo limpio.
