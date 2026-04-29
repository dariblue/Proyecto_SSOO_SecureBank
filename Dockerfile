FROM gcc:latest

# Instalar herramientas de depuración
RUN apt-get update && apt-get install -y \
    make \
    gdb \
    strace \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /securebank

# Exponer puerto TCP para Telnet (Fase II)
EXPOSE 5000

# El código fuente se monta como volumen
# Uso:
#   docker build -t securebank .
#   docker run -it --rm -p 5000:5000 -v "$(pwd):/securebank" securebank
#
# Dentro del contenedor:
#   make clean && make
#   ./init_cuentas
#   ./banco                    # Servidor
#   --- desde otra terminal ---
#   telnet 127.0.0.1 5000      # Cliente

CMD ["/bin/bash"]
