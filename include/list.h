#ifndef list_h
#define list_h


struct element {
	struct element* next;
	long data;
};

struct element* push(struct element* head, long data);

struct element* pop(struct element* head, long* data);

long list_len(struct element* head);

unsigned int is_in_list(struct element* head, long value);

#endif
