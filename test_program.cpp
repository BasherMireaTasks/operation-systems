#include <iostream>
#include <fstream>
#include <vector>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Использование: " << argv[0] 
                  << " <путь_к_библиотеке> <ключ> <входной_файл> <выходной_файл>" << std::endl;
        return 1;
    }

    const char* lib_path = argv[1];
    char key = static_cast<char>(atoi(argv[2]));
    const char* input_file = argv[3];
    const char* output_file = argv[4];

    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        std::cerr << "Ошибка загрузки библиотеки: " << dlerror() << std::endl;
        return 1;
    }

    set_key_func set_key = reinterpret_cast<set_key_func>(dlsym(handle, "set_key"));
    caesar_func caesar = reinterpret_cast<caesar_func>(dlsym(handle, "caesar"));

    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        std::cerr << "Ошибка получения символа: " << dlsym_error << std::endl;
        dlclose(handle);
        return 1;
    }

    std::ifstream in_file(input_file, std::ios::binary | std::ios::ate);
    if (!in_file.is_open()) {
        std::cerr << "Не удалось открыть входной файл: " << input_file << std::endl;
        dlclose(handle);
        return 1;
    }

    std::streamsize size = in_file.tellg();
    in_file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!in_file.read(buffer.data(), size)) {
        std::cerr << "Ошибка чтения входного файла" << std::endl;
        in_file.close();
        dlclose(handle);
        return 1;
    }
    in_file.close();

    set_key(key);
    caesar(buffer.data(), buffer.data(), size);


    std::ofstream out_file(output_file, std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "Не удалось создать выходной файл: " << output_file << std::endl;
        dlclose(handle);
        return 1;
    }

    out_file.write(buffer.data(), size);
    out_file.close();

    std::cout << "Результат записан в " << output_file << std::endl;

    dlclose(handle);

    return 0;
}