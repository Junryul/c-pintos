/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/anon.h"
#include "vm/file.h"
#include "vm/inspect.h"

// 프레임테이블, 카운트와 인덱스 전역 선언
#define FRAME_TABLE_SIZE 6144
static struct frame *frame_table[FRAME_TABLE_SIZE];
static size_t frame_count = 0;
static size_t clock_hand = 0;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
  vm_anon_init();
  vm_file_init();
#ifdef EFILESYS /* For project 4 */
  pagecache_init();
#endif
  register_inspect_intr();
  /* DO NOT MODIFY UPPER LINES. */
  /* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
    default:
      return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
                                    bool writable, vm_initializer *init,
                                    void *aux) {
  ASSERT(VM_TYPE(type) != VM_UNINIT)
  ASSERT(pg_ofs(upage) == 0);  // upage의 psize aligned 여부 확인

  struct supplemental_page_table *spt = &thread_current()->spt;
  bool (*initializer)(struct page *, enum vm_type, void *);

  // 해당 페이지가 존재하면
  if (spt_find_page(spt, upage) != NULL) return false;

  /* Check wheter the upage is already occupied or not. */
  if (spt_find_page(spt, upage) == NULL) {
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */
    struct page *page = malloc(sizeof(struct page));
    if (page == NULL) return false;

    switch (VM_TYPE(type)) {
      case VM_ANON:
        initializer = anon_initializer;
        break;
      case VM_FILE:
        initializer = file_backed_initializer;
        break;
      // case VM_PAGE_CACHE: /* for project 4 */
      default:
        free(page);
        goto err;
    }

    // page 초기화
    uninit_new(page, upage, init, type, aux, initializer);

    // page.writable 초기화
    page->writable = writable;

    /* TODO: Insert the page into the spt. */
    if (!spt_insert_page(spt, page)) {
      vm_dealloc_page(page);
      goto err;
    }
    return true;
  }

err:
  return false;
}

/* Hash Table Helpers */
static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED) {
  const struct page *p = hash_entry(e, struct page, hash_elem);
  return hash_bytes(&p->va, sizeof p->va);
}

static bool page_less(const struct hash_elem *a, const struct hash_elem *b,
                      void *aux UNUSED) {
  const struct page *pa = hash_entry(a, struct page, hash_elem);
  const struct page *pb = hash_entry(b, struct page, hash_elem);
  return pa->va < pb->va;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt, void *va) {
  ASSERT(spt != NULL);

  struct page *page = NULL;

  void *key = pg_round_down(va);
  struct page temp = {.va = key};
  struct hash_elem *e = hash_find(&spt->page_map, &temp.hash_elem);

  if (e != NULL) {
    page = hash_entry(e, struct page, hash_elem);
  }
  return page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page) {
  ASSERT(spt != NULL);
  ASSERT(page != NULL);
  ASSERT(pg_ofs(page->va) == 0);  // va가 page aligned인지 확인

  // 같은 주소에 대해 중복 삽입이 발생하는 상황에 현재는 조용한 실패. ASSERT가
  // 필요한가?

  bool succ = false;
  struct hash_elem *prev = hash_insert(&spt->page_map, &page->hash_elem);
  if (prev == NULL) {
    succ = true;
  }

  return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
  ASSERT(spt && page);
  hash_delete(&spt->page_map, &page->hash_elem);
  vm_dealloc_page(page);
  return;
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  while (true) {
    struct frame *victim = frame_table[clock_hand];

    // page 설정 시까지 시간차 존재, 혹시 모를 예외처리
    ASSERT(victim != NULL);
    ASSERT(victim->page != NULL);

    // accessed bit 확인하고 1이면 0으로 변경
    if (pml4_is_accessed(victim->page->owner->pml4, victim->page->va)) {
      pml4_set_accessed(victim->page->owner->pml4, victim->page->va, false);
    } else {
      return victim;
    }

    clock_hand = (clock_hand + 1) % frame_count;
  }
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim = vm_get_victim();

  // 스왑 아웃
  swap_out(victim->page);

  // 페이지 테이블 매핑 제거
  pml4_clear_page(victim->page->owner->pml4, victim->page->va);

  // 연결 끊기
  victim->page->frame = NULL;
  victim->page = NULL;

  // frame은 빈 상태로, 재활용 가능
  return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void) {
  void *kva = palloc_get_page(PAL_USER);

  if (kva == NULL) {
    return vm_evict_frame();
  }

  struct frame *frame = malloc(sizeof(struct frame));
  if (frame == NULL) {
    palloc_free_page(kva);
    return NULL;
  }

  frame->kva = kva;
  frame->page = NULL;

  ASSERT(frame_count < FRAME_TABLE_SIZE);
  frame_table[frame_count++] = frame;

  return frame;
}

/* 스택 주소인지 체크 */
bool is_stack_addr(void *addr, void *rsp) {
  // 잘못된 주소(NULL, 커널 가상 주소) 접근
  if (!addr || !is_user_vaddr(addr)) return false;

  // 유저 스택 주소가 아닌 경우
  if (addr >= USER_STACK) return false;

  // 유저 스택 범위를 넘어간 경우(스택 오버 플로우)
  if (pg_round_down(addr) < USER_STACK - MAX_STACK_SIZE) return false;

  // x86-64, PUSH 명령어에 의한 경우를 제외하고 스택 포인터를 벗어난 접근
  if (addr < rsp - 8) return false;

  return true;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {
  void *upage = pg_round_down(addr);

  vm_alloc_page(VM_ANON | VM_MARKER_0, upage, true);
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
                         bool user UNUSED, bool write UNUSED,
                         bool not_present UNUSED) {
  struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
  struct page *page = NULL;
  void *rsp = user ? f->rsp : thread_current()->user_rsp;

  /* TODO: Validate the fault */
  /* TODO: Your code goes here */
  // 잘못된 주소 접근
  if (!addr || !is_user_vaddr(addr)) return false;

  // writing read-only page (exception.c, page_fault 함수 참고)
  if (!not_present) return false;

  // spt에서 가상 주소 addr이 포함된 페이지 찾기
  page = spt_find_page(spt, addr);
  if (page == NULL) {
    if (is_stack_addr(addr, rsp)) {
      vm_stack_growth(addr);
      page = spt_find_page(spt, addr);
      ASSERT(page != NULL);
    } else {
      return false;
    }
  }

  // writing read-only page (not_present page 매핑 이후 확인)
  if (write && !page->writable) return false;

  return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va) {
  struct page *page = spt_find_page(&thread_current()->spt, va);

  if (page == NULL) {
    return false;
  }

  return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page *page) {
  struct frame *frame = vm_get_frame();

  /* Set links */
  frame->page = page;
  page->frame = frame;

  /* TODO: Insert page table entry to map page's VA to frame's PA. */
  if (!swap_in(page, frame->kva)) {
    page->frame = NULL;
    palloc_free_page(frame->kva);
    free(frame);
    return false;
  }

  if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva,
                     page->writable)) {
    page->frame = NULL;
    palloc_free_page(frame->kva);
    free(frame);
    return false;
  }

  return true;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt) {
  ASSERT(spt != NULL);

  bool success = hash_init(&spt->page_map, page_hash, page_less, NULL);
  if (!success) {
    PANIC("hash_init failed in supplemental_page_table_init");
  }
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {
  supplemental_page_table_init(dst);

  struct hash_iterator it;
  hash_first(&it, &src->page_map);
  while (hash_next(&it)) {
    struct hash_elem *e = hash_cur(&it);
    struct page *p = hash_entry(e, struct page, hash_elem);

    void *va = p->va;
    bool writable = p->writable;

    if (page_get_type(p) == VM_FILE) {
      continue;
    }

    if (VM_TYPE(p->operations->type) == VM_UNINIT) {
      /* 1) UNINIT Page */

      struct segment_aux *aux_src = p->uninit.aux;

      /* aux 깊은 복사 */
      struct segment_aux *aux_dst = malloc(sizeof *aux_dst);
      if (aux_dst == NULL) {
        supplemental_page_table_kill(dst);
        return false;
      }

      enum vm_type t = p->uninit.type;
      *aux_dst = *aux_src;
      if (VM_TYPE(t) == VM_FILE) {
        continue;
      }

      if (!vm_alloc_page_with_initializer(t, va, writable, p->uninit.init,
                                          aux_dst)) {
        if (VM_TYPE(t) == VM_FILE && aux_dst->file != NULL) {
          file_close(aux_dst->file);
        }
        free(aux_dst);
        supplemental_page_table_kill(dst);
        return false;
      }
    } else if (page_get_type(p) == VM_ANON) {
      /* 2) 이미 적재된 ANON 페이지 */

      /* 부모가 swap/evict 미구현 단계라면 frame은 있어야 정상 */
      ASSERT(p->frame != NULL);

      if (!vm_alloc_page_with_initializer(VM_ANON, va, writable, NULL, NULL)) {
        supplemental_page_table_kill(dst);
        return false;
      }

      if (!vm_claim_page(va)) {
        supplemental_page_table_kill(dst);
        return false;
      }

      struct page *child_p = spt_find_page(dst, va);
      memcpy(child_p->frame->kva, p->frame->kva, PGSIZE);
    } else {
      supplemental_page_table_kill(dst);
      return false;
    }
  }

  return true;
}

// void hash_action_destroy(struct hash_elem *e, void *aux UNUSED) {
//   ASSERT(e != NULL);
//   struct page *page = hash_entry(e, struct page, hash_elem);
//   ASSERT(page != NULL);
//   vm_dealloc_page(page);
// }

static void hash_action_destroy(struct hash_elem *e, void *aux UNUSED) {
  struct page *page = hash_entry(e, struct page, hash_elem);
  struct thread *t = thread_current();
  if (page_get_type(page) == VM_FILE) {
    bool mapped = pml4_get_page(t->pml4, page->va) != NULL;
    bool dirty = mapped && pml4_is_dirty(t->pml4, page->va);
    if (dirty && page->file.read_bytes > 0) {
      void *kva = page->frame->kva;  // 프레임이 있다고 가정 (evict 미구현)
      off_t ofs = page->file.ofs;
      size_t n = page->file.read_bytes;
      lock_acquire(&filesys_lock);
      file_write_at(page->file.file, kva, n, ofs);
      lock_release(&filesys_lock);
    }
    if (mapped) pml4_clear_page(t->pml4, page->va);
  }
  vm_dealloc_page(page);  // destroy(page) -> free(page)
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
  ASSERT(spt);
  hash_clear(&spt->page_map, hash_action_destroy);
}
