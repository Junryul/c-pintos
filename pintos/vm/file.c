/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

#include "filesys/filesys.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/file.h"

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  ASSERT(VM_TYPE(type) == VM_FILE);

  page->operations = &file_ops;

  struct segment_aux *aux = (struct segment_aux *)page->uninit.aux;
  ASSERT(aux != NULL);

  // 로컬 변수에 먼저 복사
  struct file *saved_file = aux->file;
  off_t saved_ofs = aux->ofs;
  size_t saved_read_bytes = aux->read_bytes;
  size_t saved_zero_bytes = aux->zero_bytes;
  bool saved_writable = aux->writable;

  // 이제 page->file에 설정 (union 문제 없음)
  struct file_page *file_page = &page->file;
  file_page->file = saved_file;
  file_page->ofs = saved_ofs;
  file_page->read_bytes = saved_read_bytes;
  file_page->zero_bytes = saved_zero_bytes;
  page->writable = saved_writable;

  return true;
}

// bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
// {
//   printf("\n[DEBUG] ========== file_backed_initializer START ==========\n");
//   printf("[DEBUG] page=%p, page->va=%p\n", page, page->va);
//   printf("[DEBUG] type=%d\n", type);
//   ASSERT(VM_TYPE(type) == VM_FILE);
//   /* Set up the handler */
//   page->operations = &file_ops;
//   struct segment_aux *aux = (struct segment_aux *)page->uninit.aux;
//   printf("[DEBUG] page->uninit.aux=%p\n", aux);
//   ASSERT(aux != NULL);
//   printf("[DEBUG] aux contents:\n");
//   printf("[DEBUG]   file=%p\n", aux->file);
//   printf("[DEBUG]   ofs=%ld\n", aux->ofs);
//   printf("[DEBUG]   read_bytes=%zu\n", aux->read_bytes);
//   printf("[DEBUG]   zero_bytes=%zu\n", aux->zero_bytes);
//   printf("[DEBUG]   writable=%d\n", aux->writable);
//   // page->uninit.aux = NULL;  // 소유권 이전
//   struct file_page *file_page = &page->file;
//   file_page->file = aux->file;
//   file_page->ofs = aux->ofs;
//   file_page->read_bytes = aux->read_bytes;
//   file_page->zero_bytes = aux->zero_bytes;
//   page->writable = aux->writable;
//   printf("[DEBUG] page->file contents set:\n");
//   printf("[DEBUG]   page->file.file=%p\n", file_page->file);
//   printf("[DEBUG]   page->file.ofs=%ld\n", file_page->ofs);
//   printf("[DEBUG]   page->file.read_bytes=%zu\n", file_page->read_bytes);
//   printf("[DEBUG]   page->file.zero_bytes=%zu\n", file_page->zero_bytes);
//   printf("[DEBUG]   page->writable=%d\n", page->writable);
//   printf("[DEBUG] file_backed_initializer SUCCESS\n");
//   printf("[DEBUG] ========== file_backed_initializer END ==========\n\n");
//   return true;
// }

/* Swap in the page by read contents from the file. */
// static bool file_backed_swap_in(struct page *page, void *kva) {
//   struct file_page *file_page UNUSED = &page->file;
//   off_t file_size =
//       file_read_at(file_page->file, kva, file_page->read_bytes,
//       file_page->ofs);
//   if (file_size != file_page->read_bytes) return false;
//   if (file_page->zero_bytes > 0) {
//     memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);
//   }
//   return true;
// }

// /* Swap out the page by writeback contents to the file. */
// static bool file_backed_swap_out(struct page *page) {
//   struct file_page *file_page UNUSED = &page->file;

//   bool is_dirty = pml4_is_dirty(page->owner->pml4, page->va);
//   if (is_dirty) {
//     off_t rewrite_file = file_write_at(file_page->file, page->frame->kva,
//                                        file_page->read_bytes,
//                                        file_page->ofs);
//     if (rewrite_file != file_page->read_bytes) return false;
//   }
//   // do_munmap(page->vma);

//   return true;
// }

static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page = &page->file;

  // NULL 체크
  if (file_page->file == NULL) {
    return false;
  }

  // 파일에서 데이터 읽기
  if (file_page->read_bytes > 0) {
    lock_acquire(&filesys_lock);
    off_t bytes_read = file_read_at(file_page->file, kva, file_page->read_bytes,
                                    file_page->ofs);
    lock_release(&filesys_lock);
    if (bytes_read != (off_t)file_page->read_bytes) {
      return false;
    }
  }

  // 나머지를 0으로 채우기
  if (file_page->zero_bytes > 0) {
    memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);
  }

  return true;
}

/* Swap out the page by writeback contents to the file. */
/* Swap out the page by writeback contents to the file. */
/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
  struct file_page *file_page = &page->file;

  if (file_page->file == NULL || page->frame == NULL) {
    return false;
  }

  // dirty bit 확인 및 write back
  bool is_dirty = pml4_is_dirty(page->owner->pml4, page->va);
  if (is_dirty && file_page->read_bytes > 0) {
    lock_acquire(&filesys_lock);
    off_t bytes_written = file_write_at(file_page->file, page->frame->kva,
                                        file_page->read_bytes, file_page->ofs);
    lock_release(&filesys_lock);
    if (bytes_written != (off_t)file_page->read_bytes) {
      return false;
    }

    // dirty bit 제거
    pml4_set_dirty(page->owner->pml4, page->va, false);
  }

  // swap_out의 역할은 여기까지!
  // pml4_clear_page와 frame 연결 해제는 vm_evict_frame에서!

  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
// static void file_backed_destroy(struct page *page) {
//   struct file_page *file_page UNUSED = &page->file;

//   struct thread *t = thread_current();
//   if (page->frame) {
//     pml4_clear_page(t->pml4, page->va);
//     page->frame->page = NULL;

//     palloc_free_page(page->frame->kva);
//     free(page->frame);
//   }
// }

static void file_backed_destroy(struct page *page) {
  struct file_page *file_page = &page->file;
  struct thread *t = thread_current();

  // frame이 있으면 (메모리에 로드되어 있으면)
  if (page->frame) {
    // dirty한지 확인하고 write back
    bool is_dirty = pml4_is_dirty(t->pml4, page->va);
    if (is_dirty && file_page->read_bytes > 0) {
      file_write_at(file_page->file, page->frame->kva, file_page->read_bytes,
                    file_page->ofs);
    }

    // page table에서 매핑 제거
    pml4_clear_page(t->pml4, page->va);

    // frame 해제
    page->frame->page = NULL;
    palloc_free_page(page->frame->kva);
    free(page->frame);
    page->frame = NULL;
  }

  // 파일 닫기
  if (file_page->file) {
    // file_close(file_page->file);
    file_page->file = NULL;
  }
}

/* Lazy load function for mmap */
static bool lazy_load_mmap(struct page *page, void *aux) {
  struct file_page *file_info = (struct file_page *)aux;

  // 파일에서 데이터 읽기
  off_t bytes_read = file_read_at(file_info->file, page->frame->kva,
                                  file_info->read_bytes, file_info->ofs);

  if (bytes_read < 0) {
    return false;
  }

  // 나머지를 0으로 채우기
  memset(page->frame->kva + bytes_read, 0, PGSIZE - bytes_read);

  return true;
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file,
              off_t offset) {
  ASSERT(addr != NULL);
  ASSERT(pg_ofs(addr) == 0);
  ASSERT(offset % PGSIZE == 0);
  struct file *fp = file_reopen(file);
  if (fp == NULL) {
    return NULL;
  }
  uint8_t *start_addr = addr;
  size_t total_page_count = (length + PGSIZE - 1) / PGSIZE;
  off_t flen = file_length(fp);
  size_t read_bytes = (flen < length) ? flen : length;
  size_t zero_bytes = PGSIZE - (read_bytes % PGSIZE);
  struct mmap_region *region = malloc(sizeof *region);
  if (region == NULL) {
    file_close(fp);
    return NULL;
  }
  region->base = addr;
  region->npages = total_page_count;
  region->file = fp;  // :불: 버그 수정: file → fp
  // :불: 디버그 5: 리스트 추가
  list_push_back(&thread_current()->mmaps, &region->elem);
  list_empty(&thread_current()->mmaps);
  int page_idx = 0;
  off_t current_offset = offset;
  while (read_bytes > 0 || zero_bytes > 0) {
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;
    struct segment_aux *aux = malloc(sizeof *aux);
    if (aux == NULL) {
      file_close(fp);
      goto rollback;
    }
    aux->file = fp;
    aux->ofs = current_offset;  // :불: 버그 수정: offset → current_offset
    aux->read_bytes = page_read_bytes;
    aux->zero_bytes = page_zero_bytes;
    aux->writable = writable;  // :불: 버그 수정: 추가!
    if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_mmap,
                                        aux)) {
      free(aux);
      goto rollback;
    }
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    addr += PGSIZE;
    current_offset += page_read_bytes;  // :불: 수정: offset을 증가시키지
                                        // 말고 별도 변수 사용
    page_idx++;
  }
  return start_addr;
rollback:
  for (size_t j = 0; j < total_page_count; j++) {
    void *page_addr =
        start_addr + (j * PGSIZE);  // :불: 버그 수정: addr → start_addr
    struct page *page = spt_find_page(&thread_current()->spt, page_addr);
    if (page != NULL) {
      spt_remove_page(&thread_current()->spt, page);
    }
  }
  list_remove(&region->elem);
  free(region);
  file_close(fp);  // :불: 추가: rollback 시 fp도 닫기
  return NULL;
}

static struct mmap_region *mmap_find_region(struct thread *t, void *base) {
  struct list_elem *e;
  for (e = list_begin(&t->mmaps); e != list_end(&t->mmaps); e = list_next(e)) {
    struct mmap_region *rg = list_entry(e, struct mmap_region, elem);
    if (rg->base == base) {
      return rg;
    }
  }
  return NULL;
}

/* Do the munmap */
void do_munmap(void *va) {
  struct thread *t = thread_current();
  struct mmap_region *rg = mmap_find_region(t, va);
  if (rg == NULL) {
    return;
  }
  uint8_t *up = (uint8_t *)rg->base;
  for (size_t i = 0; i < rg->npages; i++, up += PGSIZE) {
    struct page *p = spt_find_page(&t->spt, up);
    if (p == NULL) {
      continue;
    }

    if (VM_TYPE(page_get_type(p)) == VM_FILE && p->frame != NULL) {
      bool dirty = pml4_is_dirty(t->pml4, p->va);
      if (dirty) {
        size_t n = p->file.read_bytes;
        if (n > 0) {
          off_t written =
              file_write_at(rg->file, p->frame->kva, n, p->file.ofs);
          pml4_set_dirty(t->pml4, p->va, false);
        }
      }
      spt_remove_page(&t->spt, p);
    }
  }
  if (file_should_close(rg->file)) {
    file_close(rg->file);
  }
  list_remove(&rg->elem);
  free(rg);
}