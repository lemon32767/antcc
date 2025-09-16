unsigned char M[1<<15];

void bf(const char *p)
{
   extern int putchar(int);
   extern int getchar(void);
   static unsigned char *const Mend = M + sizeof M;
   int b;
   unsigned char *m = M;

   while (*p) {
      switch (*p) {
      case '+': ++*m; break;
      case '-': --*m; break;
      case '<': if (m == M) m = Mend; else --m; break;
      case '>': if (m == Mend) m = M; else ++m; break;
      case '.': putchar(*m); break;
      case ',': *m = getchar(); break;
      case '[':
         if (*m) break;
         b = 0;
         do switch (*p++) {
         case '[': ++b; break;
         case ']': --b; break;
         case 0: return;
         } while (b != 0);
         continue;
      case ']':
         if (!*m) break;
         b = 0;
         do switch (*p--) {
         case '[': ++b; break;
         case ']': --b; break;
         case 0: return;
         } while (b != 0);
         break;
      }
      ++p;
   }
}

int main(int argc, char **argv) {
   const char *p = "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++.";
   bf(argc < 2 ? p : argv[1]);
}
