
int comp(const void * p1, const void * p2)
{
	return (*(int *)p1 > *(int *)p2);
}
void swap1(void *p1, void * p2, int size)
{
	int i = 0;
	for (i = 0; i < size; i++)
	{
		char tmp = *((char *)p1 + i);
		*((char *)p1 + i) = *((char *)p2 + i);
		*((char *)p2 + i) = tmp;
	}
}
void qsort(void *base, int count, int size, int(*cmp)(void *, void *))
{
	int i = 0;
	int j = 0;
	for (i = 0; i < count - 1; i++)
	{
		for (j = 0; j<count - i - 1; j++)
		{
			if (comp((char *)base + j*size, (char *)base + (j + 1)*size) > 0)
			{
				swap1((char *)base + j*size, (char *)base + (j + 1)*size, size);
			}
		}
	}
}
