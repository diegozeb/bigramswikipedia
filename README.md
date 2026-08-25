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

Resultados de RendimientoPruebas ejecutadas sobre el dataset de 17.9 GB (3,000,000 de claves por worker):
Workers (N)  Conteo (s)  Merge (s)  Total (s)  Speedup Total (S)  Eficiencia (E)
1            989.89      138.83     1128.72      1.00x              100.0%
2            486.68      137.83      624.52      1.81x               90.4%
4            277.92      138.86      416.79      2.71x               67.7%
8            192.86      156.43      349.31      3.23x               40.4%

Estructura del Repositorio
├── BigramsWikipedia.cpp   # Código fuente principal en C++
├── BigramsWikipedia.exe   # Ejecutable precompilado de la aplicación
├── res_w1/                # Métricas y Top 1000 con 1 worker
├── res_w2/                # Métricas y Top 1000 con 2 workers
├── res_w4/                # Métricas y Top 1000 con 4 workers
├── res_w8/                # Métricas y Top 1000 con 8 workers
└── README.md
