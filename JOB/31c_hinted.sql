set max_parallel_workers to 0;
set max_parallel_workers_per_gather to 0;

/*+
LEADING( ((((((it1 ((it2 ((cn mc )mi_idx ))mi ))ci )n )t )mk )k ) )
HASHJOIN(ci cn it1 it2 k mc mi mi_idx mk n t)
NESTLOOP(ci cn it1 it2 mc mi mi_idx mk n t)
NESTLOOP(ci cn it1 it2 mc mi mi_idx n t)
NESTLOOP(ci cn it1 it2 mc mi mi_idx n)
NESTLOOP(ci cn it1 it2 mc mi mi_idx)
NESTLOOP(cn it1 it2 mc mi mi_idx)
NESTLOOP(cn it2 mc mi mi_idx)
NESTLOOP(cn it2 mc mi_idx)
NESTLOOP(cn mc mi_idx)
NESTLOOP(cn mc)
SEQSCAN(it1) SEQSCAN(it2) SEQSCAN(cn) INDEXSCAN(mc) INDEXSCAN(mi_idx)
INDEXSCAN(mi) INDEXSCAN(ci) INDEXSCAN(n) INDEXSCAN(t) INDEXSCAN(mk)
SEQSCAN(k)
*/
--2565516663
EXPLAIN ANALYZE 
SELECT MIN(mi.info) AS movie_budget,
       MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS writer,
       MIN(t.title) AS violent_liongate_movie
FROM cast_info AS ci,
     company_name AS cn,
     info_type AS it1,
     info_type AS it2,
     keyword AS k,
     movie_companies AS mc,
     movie_info AS mi,
     movie_info_idx AS mi_idx,
     movie_keyword AS mk,
     name AS n,
     title AS t
WHERE ci.note IN ('(writer)',
                  '(head writer)',
                  '(written by)',
                  '(story)',
                  '(story editor)')
  AND cn.name LIKE 'Lionsgate%'
  AND it1.info = 'genres'
  AND it2.info = 'votes'
  AND k.keyword IN ('murder',
                    'violence',
                    'blood',
                    'gore',
                    'death',
                    'female-nudity',
                    'hospital')
  AND mi.info IN ('Horror',
                  'Action',
                  'Sci-Fi',
                  'Thriller',
                  'Crime',
                  'War')
  AND t.id = mi.movie_id
  AND t.id = mi_idx.movie_id
  AND t.id = ci.movie_id
  AND t.id = mk.movie_id
  AND t.id = mc.movie_id
  AND ci.movie_id = mi.movie_id
  AND ci.movie_id = mi_idx.movie_id
  AND ci.movie_id = mk.movie_id
  AND ci.movie_id = mc.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND mi.movie_id = mk.movie_id
  AND mi.movie_id = mc.movie_id
  AND mi_idx.movie_id = mk.movie_id
  AND mi_idx.movie_id = mc.movie_id
  AND mk.movie_id = mc.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
  AND k.id = mk.keyword_id
  AND cn.id = mc.company_id;
