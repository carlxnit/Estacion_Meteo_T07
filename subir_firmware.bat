@echo off
setlocal

if "%~1"=="" (
    echo Uso: subir_firmware.bat NUEVA_VERSION
    echo Ejemplo: subir_firmware.bat 1.0.1
    echo.
    echo IMPORTANTE: antes de ejecutar esto, actualiza FIRMWARE_VERSION en
    echo main/system_config.h al mismo valor y vuelve a compilar (idf.py build).
    pause
    exit /b 1
)

set NEW_VERSION=%~1

echo Copiando firmware y subiendo a Git...

:: Copiar el archivo binario
echo Copiando build/estacion_meteo.bin a firmware/firmware.bin...
cp build/estacion_meteo.bin firmware/firmware.bin

:: Verificar que el archivo se copió correctamente
if exist firmware/firmware.bin (
    echo Archivo copiado correctamente.
) else (
    echo Error: No se pudo copiar el archivo.
    pause
    exit /b 1
)

:: Escribir la nueva version en version.txt
echo Actualizando firmware/version.txt a %NEW_VERSION%...
echo %NEW_VERSION%> firmware/version.txt

:: Agregar los archivos a Git
echo Agregando firmware/firmware.bin y firmware/version.txt al repositorio...
git add firmware/firmware.bin firmware/version.txt

:: Hacer commit
echo Haciendo commit...
git commit -m "Firmware update %NEW_VERSION%"

:: Subir cambios
echo Subiendo cambios al repositorio remoto...
git push origin main

echo ¡Proceso completado!
echo Firmware v%NEW_VERSION% subido correctamente.
pause
