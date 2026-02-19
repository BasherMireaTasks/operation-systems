CXX = g++
CXXFLAGS = -Wall -Wextra -pedantic -fPIC -O2
LDFLAGS = -shared

LIBRARY = libcaesar.so
TEST_PROG = test_caesar
SOURCES = caesar.cpp
TEST_SOURCE = test_program.cpp
HEADER = caesar.h

TEST_INPUT = input.txt
TEST_OUTPUT = output.txt
TEST_KEY = 42

all: $(LIBRARY)

$(LIBRARY): $(SOURCES) $(HEADER)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo "Библиотека $(LIBRARY) успешно создана"

$(TEST_PROG): $(TEST_SOURCE)
	$(CXX) $(TEST_SOURCE) -o $@ -ldl
	@echo "Тестовая программа $(TEST_PROG) успешно создана"

install: $(LIBRARY)
	sudo cp $(LIBRARY) /usr/local/lib/
	sudo ldconfig
	@echo "Библиотека установлена в /usr/local/lib/"

test: $(LIBRARY) $(TEST_PROG)
	@echo "Запуск теста..."
	@echo "=== Исходный файл ==="
	@cat $(TEST_INPUT)
	@echo ""
	@echo "=== Шифрование ==="
	./$(TEST_PROG) ./$(LIBRARY) $(TEST_KEY) $(TEST_INPUT) $(TEST_OUTPUT)
	@echo ""
	@echo "=== Зашифрованный файл (hex dump) ==="
	@xxd $(TEST_OUTPUT) | head -n 5
	@echo ""
	@echo "=== Дешифрование ==="
	./$(TEST_PROG) ./$(LIBRARY) $(TEST_KEY) $(TEST_OUTPUT) $(TEST_INPUT).decrypted
	@echo ""
	@echo "=== Расшифрованный файл ==="
	@cat $(TEST_INPUT).decrypted
	@echo ""
	@echo "=== Проверка ==="
	@if diff -q $(TEST_INPUT) $(TEST_INPUT).decrypted > /dev/null; then \
		echo "✓ Тест пройден: расшифрованные данные совпадают с исходными"; \
	else \
		echo "✗ Тест не пройден: данные не совпадают"; \
	fi

clean:
	rm -f $(LIBRARY) $(TEST_PROG) $(TEST_OUTPUT) $(TEST_INPUT).decrypted
	@echo "Проект очищен"

rebuild: clean all

.PHONY: all install test clean rebuild create_test_file