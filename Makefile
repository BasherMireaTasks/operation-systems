CXX = g++
CXXFLAGS = -Wall -Wextra -pedantic -fPIC -O2
LDFLAGS = -shared
PTHREAD_FLAGS = -pthread

LIBRARY = libcaesar.so
TEST_PROG = test_caesar
SECURE_COPY = secure_copy
SOURCES = caesar.cpp
TEST_SOURCE = test_program.cpp
HEADER = caesar.h

INPUT ?= input.txt
OUTPUT ?= output.txt
KEY ?= 42

TEST_FILE_SIZE ?= 1048576

.PHONY: all clean test create_test_file run

all: $(LIBRARY) $(SECURE_COPY) $(TEST_PROG)

$(LIBRARY): $(SOURCES) $(HEADER)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo "Библиотека $(LIBRARY) успешно создана"

$(SECURE_COPY): secure_copy.cpp $(LIBRARY)
	$(CXX) $(PTHREAD_FLAGS) -o $@ secure_copy.cpp -L. -lcaesar -Wl,-rpath,.
	@echo "Программа $(SECURE_COPY) успешно создана"

$(TEST_PROG): $(TEST_SOURCE)
	$(CXX) $(TEST_SOURCE) -o $@ -ldl
	@echo "Тестовая программа $(TEST_PROG) успешно создана"

create_test_file:
	@echo "Создание тестового файла $(INPUT) размером $(TEST_FILE_SIZE) байт..."
	dd if=/dev/urandom of=$(INPUT) bs=$(TEST_FILE_SIZE) count=1 2>/dev/null
	@echo "Файл $(INPUT) создан."

run: $(SECURE_COPY)
	@echo "Запуск ./$(SECURE_COPY) $(INPUT) $(OUTPUT) $(KEY)"
	./$(SECURE_COPY) $(INPUT) $(OUTPUT) $(KEY)
	./$(SECURE_COPY) $(OUTPUT) $(INPUT).decrypted $(KEY)

install: $(LIBRARY)
	sudo cp $(LIBRARY) /usr/local/lib/
	sudo ldconfig
	@echo "Библиотека установлена в /usr/local/lib/"

test: $(LIBRARY) $(TEST_PROG) create_test_file
	@echo "Запуск теста..."
	@echo "=== Исходный файл (первые 100 байт) ==="
	@head -c 100 $(INPUT) | xxd
	@echo ""
	@echo "=== Шифрование ==="
	./$(TEST_PROG) ./$(LIBRARY) $(KEY) $(INPUT) $(OUTPUT)
	@echo ""
	@echo "=== Зашифрованный файл (первые 100 байт) ==="
	@head -c 100 $(OUTPUT) | xxd
	@echo ""
	@echo "=== Дешифрование ==="
	./$(TEST_PROG) ./$(LIBRARY) $(KEY) $(OUTPUT) $(INPUT).decrypted
	@echo ""
	@echo "=== Расшифрованный файл (первые 100 байт) ==="
	@head -c 100 $(INPUT).decrypted | xxd
	@echo ""
	@echo "=== Проверка ==="
	@if diff -q $(INPUT) $(INPUT).decrypted > /dev/null; then \
		echo "✓ Тест пройден: расшифрованные данные совпадают с исходными"; \
	else \
		echo "✗ Тест не пройден: данные не совпадают"; \
	fi

clean:
	rm -f $(LIBRARY) $(TEST_PROG) $(SECURE_COPY) $(OUTPUT) $(INPUT).decrypted
	@echo "Проект очищен"

rebuild: clean all