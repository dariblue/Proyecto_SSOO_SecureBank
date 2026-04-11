/*
 * init_cuentas.c — Crea cuentas.dat con datos de prueba
 * Usa la estructura Cuenta definida en banco_comun.h (multi-divisa)
 */
#include "banco_comun.h"

int main(void) {
    FILE *f = fopen("cuentas.dat", "wb");
    if (!f) {
        perror("No se pudo crear cuentas.dat");
        return 1;
    }

    Cuenta cuentas[] = {
        { 1001, "John Doe",    5000.00f, 1000.00f,  800.00f },
        { 1002, "Jane Smith",  3000.00f, 2000.00f, 1500.00f },
        { 1003, "Carlos Ruiz", 7000.00f,  500.00f,  200.00f },
        { 1004, "Ana Garcia",  1500.00f,  300.00f,  100.00f },
        { 1005, "Pedro Lopez", 9000.00f, 4000.00f, 3000.00f },
    };

    int n = (int)(sizeof(cuentas) / sizeof(cuentas[0]));
    fwrite(cuentas, sizeof(Cuenta), (size_t)n, f);
    fclose(f);

    printf("cuentas.dat creado con %d cuentas de prueba.\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %d | %-15s | EUR=%.2f USD=%.2f GBP=%.2f\n",
               cuentas[i].numero_cuenta, cuentas[i].titular,
               cuentas[i].saldo_eur, cuentas[i].saldo_usd,
               cuentas[i].saldo_gbp);
    }

    return 0;
}


