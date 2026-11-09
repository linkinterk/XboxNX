/*
 * XboxNX — hilos de ayuda en los otros núcleos de la Switch (el 1 y el 2)
 * para repartir trabajo pesado, como el dibujo por software.
 */
#ifndef XBOXNX_WORKERS_H
#define XBOXNX_WORKERS_H

/* fn(arg, parte, partes) para cada parte, en paralelo; vuelve cuando todas
 * terminaron. La parte 0 la hace quien llama. */
typedef void (*WorkFn)(void *arg, int part, int nparts);

void workers_init(void);
int workers_count(void); /* partes en total (1 si no hay hilos) */
void workers_run(WorkFn fn, void *arg);

#endif
