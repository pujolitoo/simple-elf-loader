#include <print>
#include <string>
#include <sys/mman.h>
#include <fstream>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <dlfcn.h>
#include <map>

#include <elfio.hpp>

#define ROUND_UP(v, s) ((v + s - 1) & -s)
#define ROUND_DOWN(v, s) (v & -s)

#define MIN(x, y) (x < y) ? x : y

#define GB 1024*1024*1024

typedef struct {
  std::string name;
  ELFIO::Elf64_Addr value;
  ELFIO::Elf_Xword size;
  unsigned char bind, type_sym, other;
  ELFIO::Elf_Half shndx;
} ELF_SYMBOL;



std::vector<ELF_SYMBOL> symbols;
std::map<std::string, uintptr_t> stubs;

ELF_SYMBOL GetSymbolAt(ELFIO::symbol_section_accessor sym_access, int index){
  ELF_SYMBOL temp;
  sym_access.get_symbol(index, temp.name, temp.value, temp.size, temp.bind, temp.type_sym, temp.shndx, temp.other);
  return temp;
}

void StoreExportedSymbols(void* base, ELFIO::elfio& elf){
  ELFIO::symbol_section_accessor dynsymtab(elf, elf.sections[".dynsym"]);
  for (int i = 0; i < dynsymtab.get_symbols_num(); i++){
    ELF_SYMBOL sym = GetSymbolAt(dynsymtab, i);
    if(sym.value > 0 && sym.bind != ELFIO::STB_LOCAL){
      symbols.emplace_back(sym.name, (ELFIO::Elf64_Addr)base + sym.value, sym.size, sym.bind, sym.other, sym.shndx);
    }
  }
}

void PrintSymbols(const std::vector<ELF_SYMBOL>& symbols){
  for(const auto& symbol : symbols){
    std::println("{}", symbol.name);
  }
}

const ELF_SYMBOL FindSymbol(std::string_view name){
  for(const auto& symbol : symbols){
    if(symbol.name == name){
      return symbol;
    }
  }

  return ELF_SYMBOL();
}

void* LoadModuleToMemory(const char* elf_file, void** entry_point)
{
  ELFIO::elfio elffile;
  elffile.load(elf_file);

  const auto res = elffile.validate();
  if(!res.empty())
  {
    std::println("{}", res);
    return (void*)-1;
  }

  const auto phnum = elffile.segments.size();

  std::println("Found {} program headers!", phnum);

  void* base = mmap(NULL, (size_t)2*GB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  std::println("size is = {}", (size_t)2*GB);
  if(base == MAP_FAILED)
  {
    std::println("Error mapping base!");
    perror("Description");
    return (void*)-1;
  }

  munmap(base, (size_t)2*GB);

  void* module_base_address = (void*)UINT64_MAX;

  for(int i = 0; i < phnum; i++)
  {

    auto segment = elffile.segments[i];
    auto virtual_addr = segment->get_virtual_address();
    auto mem_size = segment->get_memory_size();
    auto file_size = segment->get_file_size();
    auto seg_type = segment->get_type();
    
    
    if(seg_type == ELFIO::PT_LOAD 
      && mem_size > 0)
      {
        std::printf("ph vaddr is: 0x%lx\n", virtual_addr);
        void* start = (void*) ROUND_DOWN(((uintptr_t)base + virtual_addr), 4096);
        size_t map_size = ROUND_UP(mem_size + ((uintptr_t)base + virtual_addr) - (uintptr_t)start, 4096);
        std::println("mapping on {} with size {} -> aligned {}", start, mem_size, map_size);
        void* map = mmap(start, map_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if(map == MAP_FAILED)
        {
          std::println("Error mapping segment!");
          perror("Description");
          return (void*)-1;
        }
        module_base_address = MIN(module_base_address, start);
        std::println("Mapped successfully!");
        std::println("Copying to {}, file_size = {}", (void*)((uintptr_t)base + virtual_addr), file_size);
        std::println("");

        std::memcpy((void*)((uintptr_t)base + virtual_addr), segment->get_data(), file_size);
    }
  }

  // Relocate

  StoreExportedSymbols(base, elffile);

  typedef void*(*puts_func)(const char* str);
  
  for (const auto& sect : elffile.sections)
  {
    if(sect->get_type() == ELFIO::SHT_REL || sect->get_type() == ELFIO::SHT_RELA){
      ELFIO::relocation_section_accessor relocs(elffile, sect.get());
      for(int i = 0; i < relocs.get_entries_num(); i++)
      {
        bool error = false;
        ELFIO::Elf64_Addr offset;
        ELFIO::Elf_Word symbol;
        ELFIO::Elf_Word type;
        ELFIO::Elf_Sxword addend;
        relocs.get_entry(i, offset, symbol, type, addend);

        auto symtab = elffile.sections[sect->get_link()];
        
        ELFIO::symbol_section_accessor symaccess(elffile, symtab);

        ELF_SYMBOL current_symbol = GetSymbolAt(symaccess, symbol);

        std::println("Relocation of symbol name: {}, offset {}", current_symbol.name.c_str(), (void*)offset);

        uintptr_t rel_address;
        uintptr_t replacement_value = 0;
        ELFIO::Elf64_Sword replacement_size = 8;

        switch (type)
        {
        case ELFIO::R_X86_64_RELATIVE:
          {
            rel_address = ((uintptr_t)base + offset);
            replacement_value = ((uintptr_t)base + addend);
            replacement_size = 8;
            std::println("Relocating R_X86_64_RELATIVE on {} by ({} + {}) {}!", (void*)rel_address, base, addend, (void*)replacement_value);
            break;
          }

        case ELFIO::R_X86_64_JUMP_SLOT:
        case ELFIO::R_X86_64_GLOB_DAT:
          {
            rel_address = ((uintptr_t)base + offset);
            replacement_size = 8;
            ELF_SYMBOL result = FindSymbol(current_symbol.name.c_str());
            if(result.value == 0){
              
              replacement_value = stubs[current_symbol.name];
              if(replacement_value != 0) break;
              
              std::println("Error relocating {}, symbol not found.", current_symbol.name);
              error = true;
            }else{
              replacement_value = result.value;
            }
            
            break;
          }
          break;
          
          default:
          {
            std::println("Relocation not supported!!");
            error = true;
          }
          break;
        }
        
        std::println("Relocating on {} by {}!", (void*)rel_address, (void*)replacement_value);
        std::println("");
        if(error) continue;
        
        std::memcpy((void*)rel_address, (const void*)&replacement_value, replacement_size);
      }
    }

  }
  
  std::println("All loadable segments of {} have been loaded succesfully!", elf_file);
  std::println("\n");

  if(entry_point)
    *entry_point = (void*)((ELFIO::Elf64_Addr)base + elffile.get_entry());
    
  return module_base_address;
}

void JumpEntry(void* entry, void* stack)
{
 __asm__ volatile(
    "mov %0, %%rsp\n"
    "xor %%rbp, %%rbp\n"
    "jmp *%1"
    :
    : "r"(stack), "r"(entry)
    : "rbp", "rsp"
  );
}


int main(void) {

  const char* binary_path = "./main";

  stubs["puts"] = (uintptr_t)puts;
  stubs["exit"] = (uintptr_t)exit;

  void* ep = nullptr;

  void* testlib_in_mem = LoadModuleToMemory("./testlib.so", NULL);
  void* bin_in_mem = LoadModuleToMemory(binary_path, &ep);

  std::println("Module base address is: {}", bin_in_mem);

  void* stack = mmap(NULL, 1024*1024*8, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
  void* top_stack = (void*)(((uintptr_t)stack + (1024*1024*8)) & ~0xF);

  const auto first_arg = std::filesystem::absolute(binary_path).string();

  JumpEntry(ep, top_stack);

  return 0;
}
