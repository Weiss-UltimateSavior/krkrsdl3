@echo off

cd android

echo [1/2] Building Debug APK...
call gradlew.bat assembleDebug
echo[

echo [2/2] Building Release APK...
call gradlew.bat assembleRelease
echo[
