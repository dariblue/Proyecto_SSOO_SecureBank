FROM gcc:latest

# Instalar herramientas de depuración
RUN apt-get update && apt-get install -y \
    make \
    gdb \
    strace \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /securebank

# El código fuente se monta como volumen
# Uso:
#   docker build -t securebank .
#   docker run -it --rm -v "$(pwd):/securebank" securebank
#
# Dentro del contenedor:
#   make clean && make
#   ./init_cuentas    (si no existe cuentas.dat)
#   ./banco

CMD ["/bin/bash"]
