# Plan de Documentación Académica — SecureBank

Este plan detalla el flujo de trabajo para generar una documentación de alto impacto que garantice el éxito en la entrega y la defensa presencial del proyecto.

## User Review Required

> [!IMPORTANT]
> **Tono y Estilo**: La memoria técnica usará un tono formal y académico. El Cuaderno de Bitácora usará un tono de "primera persona del plural" (estudiantes), reflejando los retos reales superados por Eugenia, Vito y Dario.

## Workflow de Redacción

### Paso 1: Redacción de la Memoria Técnica (40% de la nota)
Se redactarán las secciones estructurales siguiendo el índice aprobado:
- **Arquitectura**: Diagramación conceptual de la jerarquía de procesos (Padre-Hijos).
- **IPC**: Detalle técnico del uso de `mqueue` (asíncrono) vs `pipes` (síncrono/control).
- **Sincronización**: Justificación del uso de semáforos nombrados para la integridad de `cuentas.dat`.
- **Análisis de Rendimiento**: Explicación de la eficiencia de `fseek` sobre búsquedas repetitivas.

### Paso 2: El Relato de la Bitácora (El Factor Humano)
Creación de la narrativa sobre los tres grandes obstáculos:
1.  **Hito 1**: La incompatibilidad MacOS vs Linux (El despliegue en Docker).
2.  **Hito 2**: El descubrimiento del EBCDIC (La integridad del esqueleto y el checker).
3.  **Hito 3**: La refactorización crítica a `execv` y `fseek`.

### Paso 3: Guion de Defensa y Pruebas
Generación de una guía estratégica para la presentación en vivo:
- Secuencia de comandos para demostrar funcionalidad.
- Cómo forzar errores de seguridad para lucir el `monitor`.
- Preguntas "trampa" probables del tribunal y cómo responderlas.

## Verification Plan

### Manual Verification
- **Consistencia**: Verificar que los detalles técnicos de la memoria (nombres de semáforos, constantes como `ID_INICIAL`) coinciden exactamente con el código fuente actual.
- **Formato**: Asegurar el uso de Markdown premium con alertas (GitHub alerts) para resaltar los puntos que el profesor valorará más.
