FROM gcc:latest

# Instalar herramientas de depuración y cliente telnet
RUN apt-get update && apt-get install -y \
    make \
    gdb \
    strace \
    telnet \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /securebank

# Exponer puerto TCP para Telnet (Parte 2)
EXPOSE 5000

# El código fuente se monta como volumen
# Uso recomendado:
# 1. Construir: docker build -t securebank .
# 2. Iniciar: docker run --name servidor_banco -it --rm -v "$(pwd):/securebank" securebank
# 3. Otra terminal: docker exec -it servidor_banco bash

CMD ["/bin/bash"]
