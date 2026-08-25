Counter of bigrams in a WIkipedia txt in C++17

Procesador multihilo de alto rendimiento en C++17 diseñado para contar bigramas exactos sobre un dataset de Wikipedia de 17.9 GB, garantizando un consumo estricto de memoria RAM por debajo de los 2 GB.

Características Principales
- Arquitectura Out-of-Core: Procesa archivos masivos que superan la memoria RAM disponible mediante fragmentación dinámica y volcado a disco.
- Modelo Productor-Consumidor: Lógica multihilo desacoplada mediante una cola bloqueante concurrente (BlockingQueue).
- Optimización Fast-Path & Unicode: Validación ultrarrápida para caracteres ASCII y soporte completo para normalización UTF-8/Unicode mediante APIs nativas de Windows (GetStringTypeExW, LCMapStringEx).
- Consolidación K-Way Merge: Algoritmo de mezcla externa mediante Min-Heap de baja complejidad para combinar resultados temporales .bin sin sobrepasar descriptores de archivos del SO.

Requisitos y Ejecución
- Sistema Operativo: Windows 10 / 11 (64-bit)
- Binario precompilado: Incluido directamente en el repositorio (BigramsWikipedia.exe).

Instrucciones para PowerShell
- No se requiere compilación previa. Abre PowerShell como Administrador o dentro de un directorio autorizado y ejecuta:
  .\BigramsWikipedia.exe <ruta_archivo_entrada> <num_workers> <carpeta_salida> [max_keys]
  
Ejemplo de uso:
  .\BigramsWikipedia.exe "C:\data\wikipedia.txt" 8 "res_w8" 3000000
  
max_keys (opcional, default: 1000000): Límite de claves en RAM por hilo antes de volcar a disco. Se recomiendan 3000000 para optimizar I/O y evitar sobrepasar el límite de archivos abiertos por Windows.
