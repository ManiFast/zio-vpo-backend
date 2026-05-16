# SAV task 2 notes

## Local test as administrator

Build `Release|x64`, then run from the folder where `SAV-Service.exe` is located:

```cmd
SAV-Service.exe install
SAV-Service.exe start
```

The service starts `SAV-Antivirus.exe --tray --from-service` in user sessions.

To stop the service through the task logic, use `Exit` in the GUI tray menu or `File -> Exit`.

For cleanup after testing:

```cmd
SAV-Service.exe remove
```
