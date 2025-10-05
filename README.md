# Proyecto-1.-Sistemas-Operativos
Repositorio para el Proyecto 1. Principios de Sistemas Operativos IIS2025

Comandos Usados:
Compilar y Enlazar el Makefile:
make clean
make 

1. Inicializador: ./initializer /my_shm 10 42 ./texto_entrada.txt -> 
Espacio del Buffer = 10
Clave de Encrip = [0-255]
Ruta del archivo de lectura = ./texto_entrada.txt

2. Emisor: ./emitter /my_shm [Especificar auto/manual] 42 -> auto se leen y escriben cada 1 segundo / manual se escribe y con ENTER se procesan.
3. Receptor: ./receiver /my_shm auto 42
El receptor arma el archivo de salida, escribiendo los caracteres nuevos en el Buffer.

4. Finalizador: ./finalizer /my_shm -> 
Termina los procesos y muestra todas las estadisticas de los heavy process

- Se hizo un monitor, para ver la memoria compartida y el llenado del Buffer, el head y tail.
-> ./monitor /my_shm

