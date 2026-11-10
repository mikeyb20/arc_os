CREATE VIRTUAL TABLE docs USING fts5(title, body);
INSERT INTO docs VALUES ('Hypothermia', 'Body temperature below 35 C; warm the person slowly.');
INSERT INTO docs VALUES ('Water', 'Boil water for one minute to make it safe to drink.');
SELECT title FROM docs WHERE docs MATCH 'warm' ORDER BY rank;
