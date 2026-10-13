// SPDX-License-Identifier: BSD-3-Clause

// Nu sunt sigur la functia DIE ca am folosit-o cum trebuie
#include "osmem.h"
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include "block_meta.h"

// Am definit constantele pentru structura si dimensiunile limitei pt mmap si calloc
#define META_SIZE sizeof(struct block_meta)
#define MMAP_THRESHOLD (128 * 1024)
#define CALLOC_THRESHOLD (4 * 1024)

// definim minimul pentru realloc
#define minim (block->size < size ? block->size : size)

// Pointerul golobal pt inceputul heap-ului
void *global_base;

// Aliniem dimensiunea la 8 octeti
size_t align(size_t size)
{
	size_t size_nou = size % 8;

	if (!size_nou)
		return size;
	// Adaugam diferenta pentru a obtine un multiplu de 8
	return size + 8 - size_nou;
}

// Functia prin care descompunem un bloc in caz ca are spatiu suficient
struct block_meta *splituire(struct block_meta *fit, size_t size)
{
	// Daca exista blocul si are spatiul necesar il creem pe cel nou splituit
	if (fit && fit->size >= size + align(META_SIZE + 8)) {
		// Creeam blocul dupa blocul de potrivire
		struct block_meta *new_block = (struct block_meta *)((char *)fit + align(META_SIZE) + size);

		// Setam dimensiunea si starea noului bloc
		new_block->size = align(fit->size - size) - align(META_SIZE);
		new_block->status = STATUS_FREE;

		// Actualizam blocul pentru a reflecta dimensiunea si starea
		fit->size = size;
		fit->status = STATUS_ALLOC;

		// Conectam blocurile intre ele
		new_block->next = fit->next;
		new_block->prev = fit;
		fit->next->prev = new_block;
		fit->next = new_block;
	} else if (fit) {
		// Daca blocul nu poate fi impartit, il setam ca alocat
		fit->status = STATUS_ALLOC;
	}
	return fit;
}

// Functia prin care unim blocurile libere consecutive
void coalesce(void)
{
	struct block_meta *current = global_base;
	struct block_meta *start = global_base;

	do {
		// Daca blocurile sunt libere si este diferit de ultimul bloc fata de primul (fiind lista circulara)
		if (current->status == STATUS_FREE && current->next->status == STATUS_FREE && start != current->next) {
			// Ii adunam dimensiunile
			current->size += align(current->next->size + META_SIZE);
			struct block_meta *next_block = current->next;

			// Facem legaturile intre pointeri
			current->next = next_block->next;
			next_block->next->prev = current;
		} else {
			current = current->next;
		}
	} while (current != global_base);
}

// Functia prin care gasim un bloc liber care sa se potriveasca cu dimensiunile necesare
struct block_meta *find_best_fit(size_t size)
{
	struct block_meta *current = global_base;
	struct block_meta *best_fit = NULL;

	// Unim blocurile care sunt libere si unul langa altul
	coalesce();

	if (!current)
		return NULL;
	do {
		// In caz ca am gasit un bloc care are dimensiunea suficient de mare il retinem in best_fit
		if (current->status == STATUS_FREE && current->size >= size) {
			if (!best_fit || current->size < best_fit->size)
				best_fit = current;
		}
		current = current->next;
	} while (current != global_base);

	// In caz ca s-a gasit blocul, vedem daca putem sa il splituim
	if (best_fit)
		best_fit = splituire(best_fit, size);

	return best_fit;
}

// Functia prin care cerem spatiu in plus
struct block_meta *request_space(size_t size)
{
	struct block_meta *block;
	struct block_meta *start = global_base;

	// In caz ca ultimul bloc este free, extindem blocul respectiv
	if (start->prev->status == STATUS_FREE) {
		size_t new_size = align(size - start->prev->size);
		void *req = sbrk(new_size);

		// Verificam daca apelul sbrk a functionat
		DIE(req == (void *)-1, "a esuat sbrk");
		start->prev->status = STATUS_ALLOC;
		start->prev->size += align(new_size);
		block = start->prev;
	} else {
		// Daca ultimul bloc nu se poate extinde, creeam unul nou
		block = sbrk(align(size + META_SIZE));
		// Verificam daca apelul sbrk a functionat
		DIE(block == (void *)-1, "a esuat sbrk");
		block->size = size;
		block->status = STATUS_ALLOC;
		block->next = start;
		block->prev = start->prev;
		start->prev->next = block;
		start->prev = block;
	}

	return block;
}

// Functia prin care preinitializam heap-ul
struct block_meta *prealloc_heap(void)
{
	struct block_meta *block = NULL;

	block = sbrk(0);
	void *request = sbrk(align(MMAP_THRESHOLD));

	// Verificam daca apelul sbrk a functionat
	DIE(request == (void *)-1, "sbrk");
	// Verificam daca adresele sunt la fel
	DIE((void *)block != request, "nu corespund adresele");

	block->next = block;
	block->prev = block;
	block->size = align(MMAP_THRESHOLD) - align(META_SIZE);
	block->status = STATUS_ALLOC;
	return block;
}

void *os_malloc(size_t size)
{
	/* TODO: Implement os_malloc */
	struct block_meta *block;

	// Aliniem dimensiunea dorita
	size = align(size);

	// In caz ca dimensiunea este <= 0 nu are sens sa facem os_malloc
	if (size <= 0)
		return NULL;

	// Daca dimensiunea depaseste pragul de MMAP_THRESHOLD, folosim mmap pentru a mapa
	if (size + align(META_SIZE) >= MMAP_THRESHOLD) {
		block = mmap(NULL, align(size + META_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		// In caz ca mmap esueaza
		DIE(block == MAP_FAILED, "mmap a esuat");
		block->size = size;
		block->status = STATUS_MAPPED;
		// Am considerat ca blocurile mapate nu trebuie sa faca parte din lista
		block->next = NULL;
		block->prev = NULL;
		return (block + 1);
	}

	// Daca heap-ul nu a fost inca alocat, il alocam
	if (!global_base) {
		block = prealloc_heap();
		DIE(!block, "prealloc_heap a esuat");
		global_base = block;
	} else {
		// Altfel cautam cel mai potrivit loc liber
		block = find_best_fit(size);
		if (!block) {
			block = request_space(size);
			DIE(!block, "request_space a esuat");
		} else {
			block->status = STATUS_ALLOC;
		}
	}

	return (block + 1);
}

// Obtinem pointerulcatre metadatele blocului pe baza unui pointer
// Aceasta functie este data din documentatie
struct block_meta *get_block_ptr(void *ptr)
{
	return (struct block_meta *)ptr - 1;
}

void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (!ptr)
		return;

	// Obtinem metadatele blocului
	struct block_meta *block = get_block_ptr(ptr);

	// Ne asiguram ca blocul este alocat sau mapat inainde de eliberare
	DIE(block->status != STATUS_ALLOC && block->status != STATUS_MAPPED, "Statusul nu este valid");

	// Daca blocul este alocat cu mmap eliberam spatiul cu munmap
	// Altfel il facem status free
	if (block->status == STATUS_MAPPED)
		munmap((void *)block, align(block->size + META_SIZE));
	else
		block->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */
	struct block_meta *block;

	// Daca numarul de elemente sau dimensiunea este zero, returnam NULL
	if (nmemb <= 0 || size <= 0)
		return NULL;

	// Singura diferenta fata de os_malloc este ca avem acest total_size
	// Si ca initializam toata memoria cu 0
	size_t total_size = nmemb * size;

	total_size = align(total_size);

	if (total_size + align(META_SIZE) >= CALLOC_THRESHOLD) {
		block = mmap(NULL, align(total_size + META_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		DIE(block == MAP_FAILED, "A esuat la mapare");
		block->size = total_size;
		block->status = STATUS_MAPPED;
		block->next = NULL;
		block->prev = NULL;
		char *data_ptr = (char *)(block + 1);

		for (size_t i = 0; i < total_size; i++)
			data_ptr[i] = 0;

		return data_ptr;
	}

	if (!global_base) {
		block = prealloc_heap();
		DIE(!block, "prealloc_heap a esuat");
		global_base = block;
	} else {
		block = find_best_fit(total_size);
		if (!block) {
			block = request_space(total_size);
			DIE(!block, "request_space a esuat");
		} else {
			block->status = STATUS_ALLOC;
		}
	}

	char *data_ptr = (char *)(block + 1);

	for (size_t i = 0; i < total_size; i++)
		data_ptr[i] = 0;

	return data_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	// Daca pointerul este NULL, folosim malloc pentru a aloca spatiu din nou
	if (!ptr)
		return os_malloc(size);

	// Daca dimensiunea este 0, eliberam spatiul si returnam NULL
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	// Obtinem metadatele blocului asociat pointerului
	struct block_meta *block = get_block_ptr(ptr);

	// Daca blocul are statusul free, eliberam spatiu
	if (block->status == STATUS_FREE)
		return NULL;

	// Aliniem dimensiunea curenta
	size = align(size);

	// Daca blocul este mapat, folosim mmap
	if (block->status == STATUS_MAPPED) {
		struct block_meta *new_block;

		// Daca blocul depaseste dimensiunea MMAP_THRESHOLD, il mapam
		if (size  + align(META_SIZE) >= MMAP_THRESHOLD) {
			new_block = mmap(NULL, align(size + META_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			DIE(new_block == MAP_FAILED, "A esuat maparea");
			new_block->size = size;
			new_block->status = STATUS_MAPPED;
			new_block->next = NULL;
			new_block->prev = NULL;

			memcpy(new_block + 1, ptr, minim);
		// Altfel facem ca la os_malloc
		} else if (!global_base) {
			new_block = prealloc_heap();
			DIE(!new_block, "prealloc_heap a esuat");
			global_base = new_block;
		} else {
			new_block = find_best_fit(size);
			if (!new_block) {
				new_block = request_space(size);
				DIE(!new_block, "request_space a esuat");
			} else {
				new_block->status = STATUS_ALLOC;
			}
		}
		// Copiem informatiile din el in noul bloc
		memcpy(new_block + 1, ptr, minim);
		// Eliberam spatiul de memorie din blocul vechi
		os_free(ptr);
		return (new_block + 1);
	}

	// Daca nu este mapat, unim toate blocurile free si alaturate
	coalesce();

	// Daca blocul nu era mapat si are dimensiunea mai mare, il mapam
	if (size + align(META_SIZE) >= MMAP_THRESHOLD) {
		struct block_meta *new_block = mmap(NULL, align(size + META_SIZE),
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		DIE(new_block == MAP_FAILED, "A esuat maparea");
		new_block->size = size;
		new_block->status = STATUS_MAPPED;
		new_block->next = NULL;
		new_block->prev = NULL;

		memcpy(new_block + 1, ptr, minim);
		os_free(ptr);
		return (new_block + 1);
	}

	// Daca blocul nou trebuie sa aiba dimensiunea mai mica ca blocul curent, il splituim
	if (size <= block->size) {
		struct block_meta *new_block = block;

		new_block = splituire(new_block, size);
		return (new_block + 1);
	}

	struct block_meta *new_block = NULL;
	struct block_meta *next_block = block->next;
	struct block_meta *start = global_base;

	// Daca este ultimul bloc si avem spatiul necesar, il extindem, la fel ca la os_malloc
	if (next_block == start) {
		size_t new_size = align(size - start->prev->size);
		void *req = sbrk(new_size);

		DIE(req == (void *)-1, "A esuat sbrk");
		start->prev->status = STATUS_ALLOC;
		start->prev->size = align(start->prev->size + new_size);
		block = start->prev;
		return (block + 1);
	// Daca urmator este liber si dimensiunea totala acopera cerinta, combinam blocurile
	} else if (next_block && next_block->status == STATUS_FREE &&
		(block->size + next_block->size + align(META_SIZE) >= size)) {
		block->size += align(META_SIZE) + next_block->size;
		block->next = next_block->next;
		next_block->next->prev = block;
		new_block = splituire(block, size);
		return (new_block + 1);
	}

	// Daca nu se intampla nimic ca mai sus, incercam sa gasim cel mai bun bloc si
	// Facem ca la os_malloc
	new_block = find_best_fit(size);
	if (!new_block) {
		new_block = request_space(size);
		DIE(!new_block, "request_space a esuat");
	} else {
		new_block->status = STATUS_ALLOC;
	}
	memcpy(new_block + 1, ptr, minim);
	os_free(ptr);
	return (new_block + 1);
}
